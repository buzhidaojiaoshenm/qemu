/*
 * QTest coverage for the YAML-compiled Arm CMN configuration model.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qemu/bswap.h"

#include <sys/file.h>
#include <zlib.h>

#define CMN_CFG_BASE               0x60000000ULL
#define CMN_CFG_SIZE               0x40000000ULL
#define CMN3_HEADER_SIZE           64U
#define CMN3_ENTRY_SIZE            16U

#define CMN_CFG_SHM_PATH           "/tmp/qemu_virt_soc.arm_cmn_cfg.shm"
#define CMN_TRACE_SHM_PATH         "/tmp/qemu_virt_soc.arm_cmn_trace.shm"
#define SCMI_SHM_PATH              "/tmp/qemu_virt_soc.scmi_bridge.shm"
#define SCMI_AP_SOCK_PATH          "/tmp/qemu_virt_soc.ap.sock"
#define SCMI_SCP_SOCK_PATH         "/tmp/qemu_virt_soc.scp.sock"
#define TEST_LOCK_PATH             "/tmp/qemu_virt_soc.arm_cmn_cfg_test.lock"

typedef struct Cmn3Entry {
    uint64_t offset;
    uint64_t value;
} Cmn3Entry;

typedef enum Cmn3Corruption {
    CMN3_VALID,
    CMN3_BAD_MAGIC,
    CMN3_BAD_VERSION,
    CMN3_BAD_CRC,
    CMN3_TRUNCATED,
    CMN3_RESERVED_NONZERO,
    CMN3_DUPLICATE_OFFSET,
    CMN3_DESCENDING_OFFSET,
    CMN3_UNALIGNED_OFFSET,
    CMN3_ENTRY_PAST_EXTENT,
    CMN3_EXTENT_PAST_CFG_SIZE,
} Cmn3Corruption;

typedef struct InvalidTestData {
    const char *name;
    Cmn3Corruption corruption;
    const char *error;
} InvalidTestData;

static const Cmn3Entry valid_entries[] = {
    { 0x000000, 0x0000000000000002ULL },
    { 0x000010, 0x000000000000003bULL },
    { 0x200000, 0x0000000000000006ULL },
    { 0x200080, 0x0000000001000002ULL },
    { 0x200100, 0x0000000000210000ULL },
    { 0x210000, 0x000000000000000fULL },
    { 0x210900, 0x0110000025080000ULL },
    { 0x210908, 0x000000400000035aULL },
    { 0x280000, 0x0000000000000200ULL },
    { 0x280900, 0x0001020690000001ULL },
};

static void cleanup_fixed_paths(void)
{
    unlink(CMN_CFG_SHM_PATH);
    unlink(CMN_TRACE_SHM_PATH);
    unlink(SCMI_SHM_PATH);
    unlink(SCMI_AP_SOCK_PATH);
    unlink(SCMI_SCP_SOCK_PATH);
}

static uint32_t payload_crc(const uint8_t *payload, size_t length)
{
    uLong crc = crc32(0L, Z_NULL, 0);

    return crc32(crc, payload, length);
}

static GByteArray *make_artifact(Cmn3Corruption corruption)
{
    const size_t count = G_N_ELEMENTS(valid_entries);
    GByteArray *data = g_byte_array_sized_new(CMN3_HEADER_SIZE +
                                              count * CMN3_ENTRY_SIZE);
    uint8_t header[CMN3_HEADER_SIZE] = { 0 };
    uint8_t entry[CMN3_ENTRY_SIZE];
    uint8_t *payload;
    uint64_t first_offset = valid_entries[0].offset;
    uint64_t second_offset = valid_entries[1].offset;

    memcpy(header, "CMN3", 4);
    stw_le_p(header + 4, 1);
    stw_le_p(header + 6, CMN3_HEADER_SIZE);
    stw_le_p(header + 8, 3);
    stw_le_p(header + 10, 3);
    header[12] = 2;
    header[13] = 3;
    stl_le_p(header + 16, 0x290000);
    stl_le_p(header + 20, count);
    stl_le_p(header + 24, 0x12345678);
    memcpy(header + 32, "r2p3-00eac0", sizeof("r2p3-00eac0"));
    g_byte_array_append(data, header, sizeof(header));

    if (corruption == CMN3_UNALIGNED_OFFSET) {
        first_offset = 1;
    } else if (corruption == CMN3_DUPLICATE_OFFSET) {
        second_offset = first_offset;
    } else if (corruption == CMN3_DESCENDING_OFFSET) {
        first_offset = 0x10;
        second_offset = 0x08;
    }

    for (size_t i = 0; i < count; i++) {
        uint64_t offset = valid_entries[i].offset;

        if (i == 0) {
            offset = first_offset;
        } else if (i == 1) {
            offset = second_offset;
        }
        if (corruption == CMN3_ENTRY_PAST_EXTENT && i == count - 1) {
            offset = 0x290000;
        }
        stq_le_p(entry, offset);
        stq_le_p(entry + 8, valid_entries[i].value);
        g_byte_array_append(data, entry, sizeof(entry));
    }

    payload = data->data + CMN3_HEADER_SIZE;
    stl_le_p(data->data + 28,
             payload_crc(payload, count * CMN3_ENTRY_SIZE));

    switch (corruption) {
    case CMN3_BAD_MAGIC:
        data->data[0] = 'X';
        break;
    case CMN3_BAD_VERSION:
        stw_le_p(data->data + 4, 2);
        break;
    case CMN3_BAD_CRC:
        data->data[28] ^= 1;
        break;
    case CMN3_TRUNCATED:
        g_byte_array_set_size(data, data->len - 1);
        break;
    case CMN3_RESERVED_NONZERO:
        stw_le_p(data->data + 14, 1);
        break;
    case CMN3_EXTENT_PAST_CFG_SIZE:
        stl_le_p(data->data + 16, CMN_CFG_SIZE + 0x10000);
        break;
    default:
        break;
    }

    return data;
}

static char *write_artifact(Cmn3Corruption corruption)
{
    g_autoptr(GByteArray) data = make_artifact(corruption);
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("arm-cmn-cfg-XXXXXX.cmn3", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_file_set_contents(path, (const char *)data->data, data->len, &error);
    g_assert_no_error(error);
    return path;
}

static QTestState *start_qemu(const char *artifact_path)
{
    g_autofree char *quoted_path = g_shell_quote(artifact_path);

    cleanup_fixed_paths();
    return qtest_initf("-M qemu-scp-m7 -cpu cortex-m7 -serial none "
                       "-global arm-cmn-cfg.topology-file=%s", quoted_path);
}

static void test_seed_and_restart(void)
{
    g_autofree char *artifact_path = write_artifact(CMN3_VALID);
    QTestState *qts = start_qemu(artifact_path);

    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x000000), ==,
                    valid_entries[0].value);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x000010), ==,
                    valid_entries[1].value);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x200000), ==,
                    valid_entries[2].value);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x200100), ==,
                    valid_entries[4].value);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x210900), ==,
                    valid_entries[6].value);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x280900), ==,
                    valid_entries[9].value);

    qtest_writeq(qts, CMN_CFG_BASE + 0x4000, 0xfeedfacecafebeefULL);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x4000), ==,
                    0xfeedfacecafebeefULL);
    qtest_quit(qts);

    qts = start_qemu(artifact_path);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x4000), ==, 0);
    g_assert_cmphex(qtest_readq(qts, CMN_CFG_BASE + 0x280900), ==,
                    valid_entries[9].value);
    qtest_quit(qts);

    cleanup_fixed_paths();
    unlink(artifact_path);
}

static int spawn_daemon(const char *artifact_path, char **stderr_text,
                        char **pidfile_out)
{
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    g_autofree char *pidfile = NULL;
    g_autoptr(GError) error = NULL;
    const char *argv[18];
    int status = -1;
    int argc = 0;

    g_assert_nonnull(qemu);
    pidfile = g_strdup_printf("%s/arm-cmn-cfg-%u.pid", g_get_tmp_dir(),
                              g_random_int());
    argv[argc++] = qemu;
    argv[argc++] = "-M";
    argv[argc++] = "qemu-scp-m7";
    argv[argc++] = "-cpu";
    argv[argc++] = "cortex-m7";
    argv[argc++] = "-display";
    argv[argc++] = "none";
    argv[argc++] = "-serial";
    argv[argc++] = "none";
    argv[argc++] = "-monitor";
    argv[argc++] = "none";
    argv[argc++] = "-S";
    argv[argc++] = "-daemonize";
    argv[argc++] = "-pidfile";
    argv[argc++] = pidfile;
    if (artifact_path) {
        argv[argc++] = "-global";
        argv[argc++] = g_strdup_printf("arm-cmn-cfg.topology-file=%s",
                                       artifact_path);
    }
    argv[argc] = NULL;

    cleanup_fixed_paths();
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT,
                               NULL, NULL, NULL, stderr_text, &status,
                               &error));
    g_assert_no_error(error);
    if (artifact_path) {
        g_free((char *)argv[argc - 1]);
    }
    *pidfile_out = g_steal_pointer(&pidfile);
    return status;
}

static void stop_daemon(const char *pidfile)
{
    g_autofree char *contents = NULL;

    if (g_file_get_contents(pidfile, &contents, NULL, NULL)) {
        pid_t pid = g_ascii_strtoll(contents, NULL, 10);

        if (pid > 1) {
            kill(pid, SIGTERM);
        }
    }
    unlink(pidfile);
    cleanup_fixed_paths();
}

static void assert_startup_fails(const char *artifact_path,
                                 const char *expected_error)
{
    g_autofree char *stderr_text = NULL;
    g_autofree char *pidfile = NULL;
    int status = spawn_daemon(artifact_path, &stderr_text, &pidfile);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        stop_daemon(pidfile);
        g_error("QEMU unexpectedly accepted topology artifact");
    }
    g_assert_nonnull(strstr(stderr_text, expected_error));
    stop_daemon(pidfile);
}

static void test_missing_artifact(void)
{
    assert_startup_fails(NULL, "topology-file must be set");
}

static void test_invalid_artifact(gconstpointer opaque)
{
    const InvalidTestData *test = opaque;
    g_autofree char *artifact_path = write_artifact(test->corruption);

    assert_startup_fails(artifact_path, test->error);
    unlink(artifact_path);
}

static const InvalidTestData invalid_tests[] = {
    { "bad-magic", CMN3_BAD_MAGIC, "invalid magic" },
    { "bad-version", CMN3_BAD_VERSION, "unsupported format version" },
    { "bad-crc", CMN3_BAD_CRC, "payload CRC32 mismatch" },
    { "truncated", CMN3_TRUNCATED, "length does not match" },
    { "reserved", CMN3_RESERVED_NONZERO, "reserved field must be zero" },
    { "duplicate", CMN3_DUPLICATE_OFFSET,
      "offsets must be strictly increasing" },
    { "descending", CMN3_DESCENDING_OFFSET,
      "offsets must be strictly increasing" },
    { "unaligned", CMN3_UNALIGNED_OFFSET, "offset must be 8-byte aligned" },
    { "entry-past-extent", CMN3_ENTRY_PAST_EXTENT,
      "entry exceeds seed extent" },
    { "extent-past-cfg", CMN3_EXTENT_PAST_CFG_SIZE,
      "seed extent exceeds cfg-size" },
};

int main(int argc, char **argv)
{
    int lock_fd;
    int result;

    g_test_init(&argc, &argv, NULL);
    lock_fd = open(TEST_LOCK_PATH, O_CREAT | O_RDWR, 0600);
    g_assert_cmpint(lock_fd, >=, 0);
    g_assert_cmpint(flock(lock_fd, LOCK_EX), ==, 0);

    qtest_add_func("/arm-cmn-cfg/seed-and-restart", test_seed_and_restart);
    qtest_add_func("/arm-cmn-cfg/missing-artifact", test_missing_artifact);
    for (size_t i = 0; i < G_N_ELEMENTS(invalid_tests); i++) {
        g_autofree char *path = g_strdup_printf("/arm-cmn-cfg/invalid/%s",
                                                invalid_tests[i].name);

        qtest_add_data_func(path, &invalid_tests[i], test_invalid_artifact);
    }

    result = g_test_run();
    cleanup_fixed_paths();
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return result;
}
