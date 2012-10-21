#include "globals.hh"
#include "misc.hh"
#include "local-store.hh"
#include "immutable.hh"

#include <boost/shared_ptr.hpp>

#include <functional>
#include <queue>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>


namespace nix {


static string gcLockName = "gc.lock";
static string tempRootsDir = "temproots";
static string gcRootsDir = "gcroots";

static const int defaultGcLevel = 1000;


/* Acquire the global GC lock.  This is used to prevent new Nix
   processes from starting after the temporary root files have been
   read.  To be precise: when they try to create a new temporary root
   file, they will block until the garbage collector has finished /
   yielded the GC lock. */
int LocalStore::openGCLock(LockType lockType)
{
    Path fnGCLock = (format("%1%/%2%")
        % settings.nixStateDir % gcLockName).str();

    debug(format("acquiring global GC lock `%1%'") % fnGCLock);

    AutoCloseFD fdGCLock = open(fnGCLock.c_str(), O_RDWR | O_CREAT, 0600);
    if (fdGCLock == -1)
        throw SysError(format("opening global GC lock `%1%'") % fnGCLock);
    closeOnExec(fdGCLock);

    if (!lockFile(fdGCLock, lockType, false)) {
        printMsg(lvlError, format("waiting for the big garbage collector lock..."));
        lockFile(fdGCLock, lockType, true);
    }

    /* !!! Restrict read permission on the GC root.  Otherwise any
       process that can open the file for reading can DoS the
       collector. */

    return fdGCLock.borrow();
}


void createSymlink(const Path & link, const Path & target)
{
    /* Create directories up to `gcRoot'. */
    createDirs(dirOf(link));

    /* Create the new symlink. */
    Path tempLink = (format("%1%.tmp-%2%-%3%")
        % link % getpid() % rand()).str();
    if (symlink(target.c_str(), tempLink.c_str()) == -1)
        throw SysError(format("symlinking `%1%' to `%2%'")
            % tempLink % target);

    /* Atomically replace the old one. */
    if (rename(tempLink.c_str(), link.c_str()) == -1)
        throw SysError(format("cannot rename `%1%' to `%2%'")
            % tempLink % link);
}


void LocalStore::syncWithGC()
{
    AutoCloseFD fdGCLock = openGCLock(ltRead);
}


void LocalStore::addIndirectRoot(const Path & path)
{
    string hash = printHash32(hashString(htSHA1, path));
    Path realRoot = canonPath((format("%1%/%2%/auto/%3%")
        % settings.nixStateDir % gcRootsDir % hash).str());
    createSymlink(realRoot, path);
}


Path addPermRoot(StoreAPI & store, const Path & _storePath,
    const Path & _gcRoot, bool indirect, bool allowOutsideRootsDir)
{
    Path storePath(canonPath(_storePath));
    Path gcRoot(canonPath(_gcRoot));
    assertStorePath(storePath);

    if (isInStore(gcRoot))
        throw Error(format(
                "creating a garbage collector root (%1%) in the Nix store is forbidden "
                "(are you running nix-build inside the store?)") % gcRoot);

    if (indirect) {
        /* Don't clobber the the link if it already exists and doesn't
           point to the Nix store. */
        if (pathExists(gcRoot) && (!isLink(gcRoot) || !isInStore(readLink(gcRoot))))
            throw Error(format("cannot create symlink `%1%'; already exists") % gcRoot);
        createSymlink(gcRoot, storePath);
        store.addIndirectRoot(gcRoot);
    }

    else {
        if (!allowOutsideRootsDir) {
            Path rootsDir = canonPath((format("%1%/%2%") % settings.nixStateDir % gcRootsDir).str());

            if (string(gcRoot, 0, rootsDir.size() + 1) != rootsDir + "/")
                throw Error(format(
                    "path `%1%' is not a valid garbage collector root; "
                    "it's not in the directory `%2%'")
                    % gcRoot % rootsDir);
        }

        createSymlink(gcRoot, storePath);
    }

    /* Check that the root can be found by the garbage collector.
       !!! This can be very slow on machines that have many roots.
       Instead of reading all the roots, it would be more efficient to
       check if the root is in a directory in or linked from the
       gcroots directory. */
    if (settings.checkRootReachability) {
        Roots roots = store.findRoots();
        if (roots.find(gcRoot) == roots.end())
            printMsg(lvlError,
                format(
                    "warning: `%1%' is not in a directory where the garbage collector looks for roots; "
                    "therefore, `%2%' might be removed by the garbage collector")
                % gcRoot % storePath);
    }

    /* Grab the global GC root, causing us to block while a GC is in
       progress.  This prevents the set of permanent roots from
       increasing while a GC is in progress. */
    store.syncWithGC();

    return gcRoot;
}


/* The file to which we write our temporary roots. */
static Path fnTempRoots;
static AutoCloseFD fdTempRoots;


void LocalStore::addTempRoot(const Path & path)
{
    /* Create the temporary roots file for this process. */
    if (fdTempRoots == -1) {

        while (1) {
            Path dir = (format("%1%/%2%") % settings.nixStateDir % tempRootsDir).str();
            createDirs(dir);

            fnTempRoots = (format("%1%/%2%")
                % dir % getpid()).str();

            AutoCloseFD fdGCLock = openGCLock(ltRead);

            if (pathExists(fnTempRoots))
                /* It *must* be stale, since there can be no two
                   processes with the same pid. */
                unlink(fnTempRoots.c_str());

            fdTempRoots = openLockFile(fnTempRoots, true);

            fdGCLock.close();

            debug(format("acquiring read lock on `%1%'") % fnTempRoots);
            lockFile(fdTempRoots, ltRead, true);

            /* Check whether the garbage collector didn't get in our
               way. */
            struct stat st;
            if (fstat(fdTempRoots, &st) == -1)
                throw SysError(format("statting `%1%'") % fnTempRoots);
            if (st.st_size == 0) break;

            /* The garbage collector deleted this file before we could
               get a lock.  (It won't delete the file after we get a
               lock.)  Try again. */
        }

    }

    /* Upgrade the lock to a write lock.  This will cause us to block
       if the garbage collector is holding our lock. */
    debug(format("acquiring write lock on `%1%'") % fnTempRoots);
    lockFile(fdTempRoots, ltWrite, true);

    string s = path + '\0';
    writeFull(fdTempRoots, (const unsigned char *) s.data(), s.size());

    /* Downgrade to a read lock. */
    debug(format("downgrading to read lock on `%1%'") % fnTempRoots);
    lockFile(fdTempRoots, ltRead, true);
}


void removeTempRoots()
{
    if (fdTempRoots != -1) {
        fdTempRoots.close();
        unlink(fnTempRoots.c_str());
    }
}


/* Automatically clean up the temporary roots file when we exit. */
struct RemoveTempRoots
{
    ~RemoveTempRoots()
    {
        removeTempRoots();
    }
};

static RemoveTempRoots autoRemoveTempRoots __attribute__((unused));


typedef boost::shared_ptr<AutoCloseFD> FDPtr;
typedef list<FDPtr> FDs;


static void readTempRoots(PathSet & tempRoots, FDs & fds)
{
    /* Read the `temproots' directory for per-process temporary root
       files. */
    Strings tempRootFiles = readDirectory(
        (format("%1%/%2%") % settings.nixStateDir % tempRootsDir).str());

    foreach (Strings::iterator, i, tempRootFiles) {
        Path path = (format("%1%/%2%/%3%") % settings.nixStateDir % tempRootsDir % *i).str();

        debug(format("reading temporary root file `%1%'") % path);
        FDPtr fd(new AutoCloseFD(open(path.c_str(), O_RDWR, 0666)));
        if (*fd == -1) {
            /* It's okay if the file has disappeared. */
            if (errno == ENOENT) continue;
            throw SysError(format("opening temporary roots file `%1%'") % path);
        }

        /* This should work, but doesn't, for some reason. */
        //FDPtr fd(new AutoCloseFD(openLockFile(path, false)));
        //if (*fd == -1) continue;

        /* Try to acquire a write lock without blocking.  This can
           only succeed if the owning process has died.  In that case
           we don't care about its temporary roots. */
        if (lockFile(*fd, ltWrite, false)) {
            printMsg(lvlError, format("removing stale temporary roots file `%1%'")
                % path);
            unlink(path.c_str());
            writeFull(*fd, (const unsigned char *) "d", 1);
            continue;
        }

        /* Acquire a read lock.  This will prevent the owning process
           from upgrading to a write lock, therefore it will block in
           addTempRoot(). */
        debug(format("waiting for read lock on `%1%'") % path);
        lockFile(*fd, ltRead, true);

        /* Read the entire file. */
        string contents = readFile(*fd);

        /* Extract the roots. */
        string::size_type pos = 0, end;

        while ((end = contents.find((char) 0, pos)) != string::npos) {
            Path root(contents, pos, end - pos);
            debug(format("got temporary root `%1%'") % root);
            assertStorePath(root);
            tempRoots.insert(root);
            pos = end + 1;
        }

        fds.push_back(fd); /* keep open */
    }
}


static void findRoots(StoreAPI & store, const Path & path,
    bool recurseSymlinks, bool deleteStale, Roots & roots)
{
    try {

        struct stat st;
        if (lstat(path.c_str(), &st) == -1)
            throw SysError(format("statting `%1%'") % path);

        printMsg(lvlVomit, format("looking at `%1%'") % path);

        if (S_ISDIR(st.st_mode)) {
            Strings names = readDirectory(path);
            foreach (Strings::iterator, i, names)
                findRoots(store, path + "/" + *i, recurseSymlinks, deleteStale, roots);
        }

        else if (S_ISLNK(st.st_mode)) {
            Path target = absPath(readLink(path), dirOf(path));

            if (isInStore(target)) {
                debug(format("found root `%1%' in `%2%'")
                    % target % path);
                Path storePath = toStorePath(target);
                if (store.isValidPath(storePath))
                    roots[path] = storePath;
                else
                    printMsg(lvlInfo, format("skipping invalid root from `%1%' to `%2%'")
                        % path % storePath);
            }

            else if (recurseSymlinks) {
                if (pathExists(target))
                    findRoots(store, target, false, deleteStale, roots);
                else if (deleteStale) {
                    printMsg(lvlInfo, format("removing stale link from `%1%' to `%2%'") % path % target);
                    /* Note that we only delete when recursing, i.e.,
                       when we are still in the `gcroots' tree.  We
                       never delete stuff outside that tree. */
                    unlink(path.c_str());
                }
            }
        }

    }

    catch (SysError & e) {
        /* We only ignore permanent failures. */
        if (e.errNo == EACCES || e.errNo == ENOENT || e.errNo == ENOTDIR)
            printMsg(lvlInfo, format("cannot read potential root `%1%'") % path);
        else
            throw;
    }
}


static Roots findRoots(StoreAPI & store, bool deleteStale)
{
    Roots roots;
    Path rootsDir = canonPath((format("%1%/%2%") % settings.nixStateDir % gcRootsDir).str());
    findRoots(store, rootsDir, true, deleteStale, roots);
    return roots;
}


Roots LocalStore::findRoots()
{
    return nix::findRoots(*this, false);
}


static void addAdditionalRoots(StoreAPI & store, PathSet & roots)
{
    Path rootFinder = getEnv("NIX_ROOT_FINDER",
        settings.nixLibexecDir + "/nix/find-runtime-roots.pl");

    if (rootFinder.empty()) return;

    debug(format("executing `%1%' to find additional roots") % rootFinder);

    string result = runProgram(rootFinder);

    Strings paths = tokenizeString<Strings>(result, "\n");

    foreach (Strings::iterator, i, paths) {
        if (isInStore(*i)) {
            Path path = toStorePath(*i);
            if (roots.find(path) == roots.end() && store.isValidPath(path)) {
                debug(format("got additional root `%1%'") % path);
                roots.insert(path);
            }
        }
    }
}


struct GCLimitReached { };


struct LocalStore::GCState
{
    GCOptions options;
    GCResults & results;
    PathSet roots;
    PathSet tempRoots;
    PathSet deleted;
    PathSet live;
    PathSet busy;
    PathSet invalidated;
    bool gcKeepOutputs;
    bool gcKeepDerivations;
    unsigned long long bytesInvalidated;
    GCState(GCResults & results_) : results(results_), bytesInvalidated(0) { }
};


static bool shouldDelete(GCOptions::GCAction action)
{
    return action == GCOptions::gcDeleteDead
        || action == GCOptions::gcDeleteSpecific;
}


bool LocalStore::isActiveTempFile(const GCState & state,
    const Path & path, const string & suffix)
{
    return hasSuffix(path, suffix)
        && state.tempRoots.find(string(path, 0, path.size() - suffix.size())) != state.tempRoots.end();
}


void LocalStore::deleteGarbage(GCState & state, const Path & path)
{
    printMsg(lvlInfo, format("deleting `%1%'") % path);
    unsigned long long bytesFreed;
    deletePathWrapped(path, bytesFreed);
    state.results.bytesFreed += bytesFreed;
}


bool LocalStore::tryToDelete(GCState & state, const Path & path)
{
    checkInterrupt();

    if (path == linksDir) return true;

    struct stat st;
    if (lstat(path.c_str(), &st)) {
        if (errno == ENOENT) return true;
        throw SysError(format("getting status of %1%") % path);
    }

    if (state.deleted.find(path) != state.deleted.end()) return true;
    if (state.live.find(path) != state.live.end()) return false;

    startNest(nest, lvlDebug, format("considering whether to delete `%1%'") % path);

    /* If gc-keep-outputs and gc-keep-derivations are both set, we can
       have cycles in the liveness graph, so we need to treat such
       strongly connected components as a single unit (‘paths’).  That
       is, we can delete the elements of ‘paths’ only if all referrers
       of ‘paths’ are garbage. */
    PathSet paths, referrers;
    Paths pathsSorted;

    if (isValidPath(path)) {

        /* Add derivers and outputs of ‘path’ to ‘paths’. */
        PathSet todo;
        todo.insert(path);
        while (!todo.empty()) {
            Path p = *todo.begin();
            assertStorePath(p);
            todo.erase(p);
            if (paths.find(p) != paths.end()) continue;
            paths.insert(p);
            /* If gc-keep-derivations is set and this is a derivation,
               then don't delete the derivation if any of the outputs
               are live. */
            if (state.gcKeepDerivations && isDerivation(p)) {
                PathSet outputs = queryDerivationOutputs(p);
                foreach (PathSet::iterator, i, outputs)
                    if (isValidPath(*i)) todo.insert(*i);
            }
            /* If gc-keep-outputs is set, then don't delete this path
               if there are derivers of this path that are not
               garbage. */
            if (state.gcKeepOutputs) {
                PathSet derivers = queryValidDerivers(p);
                foreach (PathSet::iterator, i, derivers) todo.insert(*i);
            }
        }
    }

    else {
        /* A lock file belonging to a path that we're building right
           now isn't garbage. */
        if (isActiveTempFile(state, path, ".lock")) return false;

        /* Don't delete .chroot directories for derivations that are
           currently being built. */
        if (isActiveTempFile(state, path, ".chroot")) return false;

        paths.insert(path);
    }

    /* Check if any path in ‘paths’ is a root. */
    foreach (PathSet::iterator, i, paths)
        if (state.roots.find(*i) != state.roots.end()) {
            printMsg(lvlDebug, format("cannot delete `%1%' because it's a root") % *i);
            goto isLive;
        }

    /* Recursively try to delete the referrers of this strongly
       connected component.  If any referrer can't be deleted, then
       these paths can't be deleted either. */
    foreach (PathSet::iterator, i, paths)
        if (isValidPath(*i)) queryReferrers(*i, referrers);

    foreach (PathSet::iterator, i, referrers)
        if (paths.find(*i) == paths.end() && !tryToDelete(state, *i)) {
            printMsg(lvlDebug, format("cannot delete `%1%' because it has live referrers") % *i);
            goto isLive;
        }

    /* The paths are garbage, so delete them. */
    pathsSorted = topoSortPaths(*this, paths);
    foreach (Paths::iterator, i, pathsSorted) {
        if (shouldDelete(state.options.action)) {

            /* If it's a valid path that's not a regular file or
               symlink, invalidate it, rename it, and schedule it for
               deletion.  The renaming is to ensure that later (when
               we're not holding the global GC lock) we can delete the
               path without being afraid that the path has become
               alive again.  Otherwise delete it right away. */
            if (isValidPath(*i)) {
                if (S_ISDIR(st.st_mode)) {
                    printMsg(lvlInfo, format("invalidating `%1%'") % *i);
                    // Estimate the amount freed using the narSize field.
                    state.bytesInvalidated += queryPathInfo(*i).narSize;
                    invalidatePathChecked(*i);
                    makeMutable(i->c_str());
                    // Mac OS X cannot rename directories if they are read-only.
                    if (chmod(i->c_str(), st.st_mode | S_IWUSR) == -1)
                        throw SysError(format("making `%1%' writable") % *i);
                    Path tmp = (format("%1%-gc-%2%") % *i % getpid()).str();
                    if (rename(i->c_str(), tmp.c_str()))
                        throw SysError(format("unable to rename `%1%' to `%2%'") % *i % tmp);
                    state.invalidated.insert(tmp);
                } else {
                    invalidatePathChecked(*i);
                    deleteGarbage(state, *i);
                }
            } else
                deleteGarbage(state, *i);

            if (state.results.bytesFreed + state.bytesInvalidated > state.options.maxFreed) {
                printMsg(lvlInfo, format("deleted or invalidated more than %1% bytes; stopping") % state.options.maxFreed);
                throw GCLimitReached();
            }

        } else
            printMsg(lvlTalkative, format("would delete `%1%'") % *i);

        state.deleted.insert(*i);
        if (state.options.action != GCOptions::gcReturnLive)
            state.results.paths.insert(*i);
    }

    return true;

 isLive:
    foreach (PathSet::iterator, i, paths) {
        state.live.insert(*i);
        if (state.options.action == GCOptions::gcReturnLive)
            state.results.paths.insert(*i);
    }
    return false;
}


/* Unlink all files in /nix/store/.links that have a link count of 1,
   which indicates that there are no other links and so they can be
   safely deleted.  FIXME: race condition with optimisePath(): we
   might see a link count of 1 just before optimisePath() increases
   the link count. */
void LocalStore::removeUnusedLinks(const GCState & state)
{
    AutoCloseDir dir = opendir(linksDir.c_str());
    if (!dir) throw SysError(format("opening directory `%1%'") % linksDir);

    long long actualSize = 0, unsharedSize = 0;

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir)) {
        checkInterrupt();
        string name = dirent->d_name;
        if (name == "." || name == "..") continue;
        Path path = linksDir + "/" + name;

        struct stat st;
        if (lstat(path.c_str(), &st) == -1)
            throw SysError(format("statting `%1%'") % path);

        if (st.st_nlink != 1) {
            unsigned long long size = st.st_blocks * 512ULL;
            actualSize += size;
            unsharedSize += (st.st_nlink - 1) * size;
            continue;
        }

        printMsg(lvlTalkative, format("deleting unused link `%1%'") % path);

        if (unlink(path.c_str()) == -1)
            throw SysError(format("deleting `%1%'") % path);

        state.results.bytesFreed += st.st_blocks * 512;
    }

    struct stat st;
    if (stat(linksDir.c_str(), &st) == -1)
        throw SysError(format("statting `%1%'") % linksDir);
    long long overhead = st.st_blocks * 512ULL;

    printMsg(lvlInfo, format("note: currently hard linking saves %.2f MiB")
        % ((unsharedSize - actualSize - overhead) / (1024.0 * 1024.0)));
}


void LocalStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    GCState state(results);
    state.options = options;

    state.gcKeepOutputs = settings.gcKeepOutputs;
    state.gcKeepDerivations = settings.gcKeepDerivations;

    /* Using `--ignore-liveness' with `--delete' can have unintended
       consequences if `gc-keep-outputs' or `gc-keep-derivations' are
       true (the garbage collector will recurse into deleting the
       outputs or derivers, respectively).  So disable them. */
    if (options.action == GCOptions::gcDeleteSpecific && options.ignoreLiveness) {
        state.gcKeepOutputs = false;
        state.gcKeepDerivations = false;
    }

    /* Acquire the global GC root.  This prevents
       a) New roots from being added.
       b) Processes from creating new temporary root files. */
    AutoCloseFD fdGCLock = openGCLock(ltWrite);

    /* Find the roots.  Since we've grabbed the GC lock, the set of
       permanent roots cannot increase now. */
    printMsg(lvlError, format("finding garbage collector roots..."));
    Roots rootMap = options.ignoreLiveness ? Roots() : nix::findRoots(*this, true);

    foreach (Roots::iterator, i, rootMap) state.roots.insert(i->second);

    /* Add additional roots returned by the program specified by the
       NIX_ROOT_FINDER environment variable.  This is typically used
       to add running programs to the set of roots (to prevent them
       from being garbage collected). */
    if (!options.ignoreLiveness)
        addAdditionalRoots(*this, state.roots);

    /* Read the temporary roots.  This acquires read locks on all
       per-process temporary root files.  So after this point no paths
       can be added to the set of temporary roots. */
    FDs fds;
    readTempRoots(state.tempRoots, fds);
    state.roots.insert(state.tempRoots.begin(), state.tempRoots.end());

    /* After this point the set of roots or temporary roots cannot
       increase, since we hold locks on everything.  So everything
       that is not reachable from `roots'. */

    /* Now either delete all garbage paths, or just the specified
       paths (for gcDeleteSpecific). */

    if (options.action == GCOptions::gcDeleteSpecific) {

        foreach (PathSet::iterator, i, options.pathsToDelete) {
            assertStorePath(*i);
            if (!tryToDelete(state, *i))
                throw Error(format("cannot delete path `%1%' since it is still alive") % *i);
        }

    } else if (options.maxFreed > 0) {

        if (shouldDelete(state.options.action))
            printMsg(lvlError, format("deleting garbage..."));
        else
            printMsg(lvlError, format("determining live/dead paths..."));

        try {

            AutoCloseDir dir = opendir(settings.nixStore.c_str());
            if (!dir) throw SysError(format("opening directory `%1%'") % settings.nixStore);

            /* Read the store and immediately delete all paths that
               aren't valid.  When using --max-freed etc., deleting
               invalid paths is preferred over deleting unreachable
               paths, since unreachable paths could become reachable
               again.  We don't use readDirectory() here so that GCing
               can start faster. */
            Paths entries;
            struct dirent * dirent;
            while (errno = 0, dirent = readdir(dir)) {
                checkInterrupt();
                string name = dirent->d_name;
                if (name == "." || name == "..") continue;
                Path path = settings.nixStore + "/" + name;
                if (isValidPath(path))
                    entries.push_back(path);
                else
                    tryToDelete(state, path);
            }

            dir.close();

            /* Now delete the unreachable valid paths.  Randomise the
               order in which we delete entries to make the collector
               less biased towards deleting paths that come
               alphabetically first (e.g. /nix/store/000...).  This
               matters when using --max-freed etc. */
            vector<Path> entries_(entries.begin(), entries.end());
            random_shuffle(entries_.begin(), entries_.end());

            foreach (vector<Path>::iterator, i, entries_)
                tryToDelete(state, *i);

        } catch (GCLimitReached & e) {
        }
    }

    /* Allow other processes to add to the store from here on. */
    fdGCLock.close();

    /* Delete the invalidated paths now that the lock has been
       released. */
    foreach (PathSet::iterator, i, state.invalidated)
        deleteGarbage(state, *i);

    /* Clean up the links directory. */
    if (options.action == GCOptions::gcDeleteDead || options.action == GCOptions::gcDeleteSpecific) {
        printMsg(lvlError, format("deleting unused links..."));
        removeUnusedLinks(state);
    }

    /* While we're at it, vacuum the database. */
    if (options.action == GCOptions::gcDeleteDead) vacuumDB();
}


}
