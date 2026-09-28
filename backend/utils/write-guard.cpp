// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file write-guard.cpp
 * @brief Landlock implementation of the optional shell write restriction.
 *        Only write-type file system rights are handled, so reading and
 *        executing stay unrestricted everywhere.
 * @layer Utility
 * @dependencies Qt6::Core, Linux Landlock (kernel 5.13+)
 */

#include "write-guard.h"

#include <QDir>
#include <QFileInfo>
#include <string>
#include <vector>

#if defined(Q_OS_LINUX) && __has_include(<linux/landlock.h>)
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__NR_landlock_create_ruleset) && defined(__NR_landlock_add_rule) &&                    \
    defined(__NR_landlock_restrict_self)
#define VERZETA_HAVE_LANDLOCK 1
#endif
#endif

namespace Verzeta::WriteGuard {

#ifdef VERZETA_HAVE_LANDLOCK

namespace {

// Write-type rights available at each ABI version. Read and execute rights
// are deliberately not handled, so they remain allowed everywhere.
__u64 handledWriteRights(int abi) {
    __u64 rights = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                   LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                   LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                   LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                   LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2)
        rights |= LANDLOCK_ACCESS_FS_REFER;  // rename/link across folders
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3)
        rights |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    return rights;
}

void writeStderr(const char* msg) {
    size_t n = 0;
    while (msg[n])
        ++n;
    ssize_t ignored = ::write(STDERR_FILENO, msg, n);
    (void)ignored;
}

}  // namespace

int abiVersion() {
    static const int abi = [] {
        const long v =
            syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
        return v > 0 ? int(v) : 0;
    }();
    return abi;
}

std::function<void()> childSetup(const QStringList& writableRoots) {
    const int abi = abiVersion();
    if (abi <= 0)
        return [] {};

    // Resolve everything in the parent: the child may not allocate.
    std::vector<std::string> roots;
    for (const QString& r : writableRoots) {
        if (r.isEmpty())
            continue;
        const QString canonical = QFileInfo(r).canonicalFilePath();
        if (canonical.isEmpty())
            continue;  // missing: skip
        const std::string path = QDir::toNativeSeparators(canonical).toStdString();
        bool seen = false;
        for (const std::string& p : roots)
            seen = seen || (p == path);
        if (!seen)
            roots.push_back(path);
    }
    const __u64 rights = handledWriteRights(abi);

    return [roots, rights]() {
        struct landlock_ruleset_attr attr = {};
        attr.handled_access_fs = rights;
        const int rulesetFd = int(syscall(__NR_landlock_create_ruleset, &attr, sizeof(attr), 0));
        if (rulesetFd < 0) {
            writeStderr(
                "Write protection could not be set up (Landlock ruleset); command not run.\n");
            ::_exit(126);
        }
        for (const std::string& p : roots) {
            const int fd = ::open(p.c_str(), O_PATH | O_CLOEXEC);
            if (fd < 0)
                continue;
            struct landlock_path_beneath_attr pb = {};
            pb.parent_fd = fd;
            pb.allowed_access = rights;
            if (syscall(__NR_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0) !=
                0) {
                // A regular file (not a folder) cannot take folder rights;
                // allow only what applies to files.
                pb.allowed_access = rights & (LANDLOCK_ACCESS_FS_WRITE_FILE
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
                                              | (rights & LANDLOCK_ACCESS_FS_TRUNCATE)
#endif
                                             );
                syscall(__NR_landlock_add_rule, rulesetFd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
            }
            ::close(fd);
        }
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            syscall(__NR_landlock_restrict_self, rulesetFd, 0) != 0) {
            writeStderr("Write protection could not be applied; command not run.\n");
            ::_exit(126);
        }
        ::close(rulesetFd);
    };
}

#else  // no Landlock on this platform or build

int abiVersion() {
    return 0;
}

std::function<void()> childSetup(const QStringList& writableRoots) {
    Q_UNUSED(writableRoots);
    return [] {};
}

#endif

bool available() {
    return abiVersion() > 0;
}

}  // namespace Verzeta::WriteGuard
