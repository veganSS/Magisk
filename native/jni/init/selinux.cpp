#include <sys/mount.h>

#include <magisk.hpp>
#include <magiskpolicy.hpp>
#include <utils.hpp>

#include "init.hpp"

using namespace std;

void MagiskInit::patch_sepolicy(const char *file) {
    LOGD("Patching monolithic policy\n");
    auto sepol = unique_ptr<sepolicy>(sepolicy::from_file("/sepolicy"));

    sepol->magisk_rules();

    // Custom rules
    if (!custom_rules_dir.empty()) {
        if (auto dir = xopen_dir(custom_rules_dir.data())) {
            for (dirent *entry; (entry = xreaddir(dir.get()));) {
                auto rule = custom_rules_dir + "/" + entry->d_name + "/sepolicy.rule";
                if (xaccess(rule.data(), R_OK) == 0) {
                    LOGD("Loading custom sepolicy patch: [%s]\n", rule.data());
                    sepol->load_rule_file(rule.data());
                }
            }
        }
    }

    LOGD("Dumping sepolicy to: [%s]\n", file);
    sepol->to_file(file);

    // Remove OnePlus stupid debug sepolicy and use our own
    if (access("/sepolicy_debug", F_OK) == 0) {
        unlink("/sepolicy_debug");
        link("/sepolicy", "/sepolicy_debug");
    }
}

#define MOCK_LOAD      SELINUXMOCK "/load"
#define MOCK_ENFORCE   SELINUXMOCK "/enforce"
#define MOCK_COMPAT    SELINUXMOCK "/compatible"
#define REAL_SELINUXFS SELINUXMOCK "/fs"

void MagiskInit::hijack_sepolicy() {
    // Read all custom rules into memory
    string rules;
    if (!custom_rules_dir.empty()) {
        if (auto dir = xopen_dir(custom_rules_dir.data())) {
            for (dirent *entry; (entry = xreaddir(dir.get()));) {
                auto rule_file = custom_rules_dir + "/" + entry->d_name + "/sepolicy.rule";
                if (xaccess(rule_file.data(), R_OK) == 0) {
                    LOGD("Load custom sepolicy patch: [%s]\n", rule_file.data());
                    full_read(rule_file.data(), rules);
                    rules += '\n';
                }
            }
        }
    }

    // Hijack the "load" and "enforce" node in selinuxfs to manipulate
    // the actual sepolicy being loaded into the kernel
    xmkdir(SELINUXMOCK, 0);
    auto hijack = [] {
        LOGD("Hijack [" SELINUX_LOAD "] and [" SELINUX_ENFORCE "]\n");
        mkfifo(MOCK_LOAD, 0600);
        mkfifo(MOCK_ENFORCE, 0644);
        xmount(MOCK_LOAD, SELINUX_LOAD, nullptr, MS_BIND, nullptr);
        xmount(MOCK_ENFORCE, SELINUX_ENFORCE, nullptr, MS_BIND, nullptr);
    };

    string dt_compat;
    if (access(SELINUX_ENFORCE, F_OK) != 0) {
        // selinuxfs not mounted yet. Hijack the dt fstab nodes first
        // and let the original init mount selinuxfs for us
        // This only happens on Android 8.0 - 9.0

        // Preserve sysfs and procfs for hijacking
        mount_list.erase(std::remove_if(
                mount_list.begin(), mount_list.end(),
                [](const string &s) { return s == "/proc" || s == "/sys"; }), mount_list.end());

        // Remount procfs with proper options
        xmount(nullptr, "/proc", nullptr, MS_REMOUNT, "hidepid=2,gid=3009");

        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/fstab/compatible", config->dt_dir);
        dt_compat = full_read(buf);

        LOGD("Hijack [%s]\n", buf);
        mkfifo(MOCK_COMPAT, 0444);
        xmount(MOCK_COMPAT, buf, nullptr, MS_BIND, nullptr);
    } else {
        hijack();
    }

    // Create a new process waiting for init operations
    if (xfork()) {
        // In parent, return and continue boot process
        return;
    }

    if (!dt_compat.empty()) {
        // This open will block until init calls DoFirstStageMount
        // The only purpose here is actually to wait for init to mount selinuxfs for us
        int fd = xopen(MOCK_COMPAT, O_WRONLY);

        char buf[4096];
        snprintf(buf, sizeof(buf), "%s/fstab/compatible", config->dt_dir);
        xumount2(buf, MNT_DETACH);

        hijack();
        xwrite(fd, dt_compat.data(), dt_compat.size());
        close(fd);
    }

    // Read full sepolicy
    int fd = xopen(MOCK_LOAD, O_RDONLY);
    string policy = fd_full_read(fd);
    close(fd);
    auto sepol = unique_ptr<sepolicy>(sepolicy::from_data(policy.data(), policy.length()));

    sepol->magisk_rules();
    sepol->load_rules(rules);

    // Mount selinuxfs to another path
    xmkdir(REAL_SELINUXFS, 0755);
    xmount("selinuxfs", REAL_SELINUXFS, "selinuxfs", 0, nullptr);

    // This open will block until init calls security_getenforce
    fd = xopen(MOCK_ENFORCE, O_WRONLY);

    // Cleanup the hijacks
    umount2("/init", MNT_DETACH);
    xumount2(SELINUX_LOAD, MNT_DETACH);
    xumount2(SELINUX_ENFORCE, MNT_DETACH);

    // Load patched policy
    sepol->to_file(REAL_SELINUXFS "/load");

    // Write to mock "enforce" ONLY after sepolicy is loaded. We need to make sure
    // the actual init process is blocked until sepolicy is loaded, or else
    // restorecon will fail and re-exec won't change context, causing boot failure.
    // We (ab)use the fact that security_getenforce reads the "enforce" file, and
    // because it has been replaced with our FIFO file, init will block until we
    // write something into the pipe, effectively hijacking its control flow.

    xwrite(fd, "0", 1);
    close(fd);

    // At this point, the init process will be unblocked
    // and continue on with restorecon + re-exec.

    // Terminate process
    exit(0);
}
