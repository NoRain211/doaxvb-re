#include "kernel_abi.h"
#include "save_transaction.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#endif

enum {
    TEST_BASE = 0x2b000000u,
    TEST_SIZE = 0x1000u,
    TEST_STACK = TEST_BASE + 0x100u,
    TEST_HANDLE = TEST_BASE + 0x200u,
    TEST_IOSB = TEST_BASE + 0x210u,
    TEST_ATTRIBUTES = TEST_BASE + 0x300u,
    TEST_NAME = TEST_BASE + 0x320u,
    TEST_PATH = TEST_BASE + 0x400u,
    TEST_BUFFER = TEST_BASE + 0x600u,
    TEST_INFORMATION = TEST_BASE + 0x700u,
};

static int expect(const char *label, int condition)
{
    if (condition) return 1;
    fprintf(stderr, "kernel file save: %s failed\n", label);
    return 0;
}

static uint32_t invoke(uint32_t ordinal, const uint32_t *args,
    unsigned count, int *passed)
{
    RecompFunction function = recomp_kernel_file(ordinal);
    *passed &= expect("ordinal registered", function != NULL);
    if (function == NULL) return 0xc0000002u;
    *recomp_memory_u32(TEST_STACK) = 0x0010abcdu;
    for (unsigned i = 0u; i < count; ++i)
        *recomp_memory_u32(TEST_STACK + 4u * (i + 1u)) = args[i];
    recomp_runtime.registers.esp = TEST_STACK;
    recomp_runtime.registers.eax = 0xccccccccu;
    function();
    *passed &= expect("stdcall ESP", recomp_runtime.registers.esp ==
        TEST_STACK + 4u * (count + 1u));
    *passed &= expect("return address preserved",
        *recomp_memory_u32(TEST_STACK) == 0x0010abcdu);
    return recomp_runtime.registers.eax;
}

static void set_path(const char *path)
{
    uint32_t length = (uint32_t)strlen(path);
    memcpy(recomp_memory_i8(TEST_PATH), path, length + 1u);
    *recomp_memory_u32(TEST_NAME) = length | ((length + 1u) << 16u);
    *recomp_memory_u32(TEST_NAME + 4u) = TEST_PATH;
    *recomp_memory_u32(TEST_ATTRIBUTES) = 0u;
    *recomp_memory_u32(TEST_ATTRIBUTES + 4u) = TEST_NAME;
    *recomp_memory_u32(TEST_ATTRIBUTES + 8u) = 0u;
}

static uint32_t create_file(const char *path, uint32_t access,
    uint32_t disposition, uint32_t *status, int *passed)
{
    const uint32_t args[] = {TEST_HANDLE, access, TEST_ATTRIBUTES, TEST_IOSB,
        0u, 0u, 3u, disposition, 0u};
    set_path(path);
    *status = invoke(190u, args, 9u, passed);
    *passed &= expect("create IOSB status", *recomp_memory_u32(TEST_IOSB) == *status);
    return *recomp_memory_u32(TEST_HANDLE);
}

static uint32_t open_existing(const char *path, uint32_t access, uint32_t share,
    unsigned api, uint32_t options, int *passed)
{
    const uint32_t open_args[] = {TEST_HANDLE, access, TEST_ATTRIBUTES,
        TEST_IOSB, share, options};
    const uint32_t create_args[] = {TEST_HANDLE, access, TEST_ATTRIBUTES,
        TEST_IOSB, 0u, 0u, share, 1u, options};
    set_path(path);
    uint32_t status = api == 0u ? invoke(202u, open_args, 6u, passed)
                               : invoke(190u, create_args, 9u, passed);
    *passed &= expect("open-existing IOSB status", *recomp_memory_u32(TEST_IOSB) == status);
    return status;
}

static uint32_t write_file(uint32_t handle, const char *bytes,
    uint32_t length, int *passed)
{
    const uint32_t args[] = {handle, 0u, 0u, 0u, TEST_IOSB,
        TEST_BUFFER, length, 0u};
    uint32_t status;
    memcpy(recomp_memory_i8(TEST_BUFFER), bytes, length);
    status = invoke(236u, args, 8u, passed);
    *passed &= expect("write IOSB status", *recomp_memory_u32(TEST_IOSB) == status);
    *passed &= expect("write IOSB byte count",
        *recomp_memory_u32(TEST_IOSB + 4u) == (status == 0u ? length : 0u));
    return status;
}

static uint32_t set_information(uint32_t handle, uint32_t kind,
    uint32_t value, uint32_t length, int *passed)
{
    const uint32_t args[] = {handle, TEST_IOSB, TEST_INFORMATION, length, kind};
    uint32_t status;
    *recomp_memory_u32(TEST_INFORMATION) = value;
    *recomp_memory_u32(TEST_INFORMATION + 4u) = 0u;
    status = invoke(226u, args, 5u, passed);
    *passed &= expect("set-information IOSB status",
        *recomp_memory_u32(TEST_IOSB) == status);
    return status;
}

static uint32_t set_file_attributes(uint32_t handle, uint32_t attributes,
    int *passed)
{
    const uint32_t args[] = {handle, TEST_IOSB, TEST_INFORMATION, 0x28u, 4u};
    memset(recomp_memory_i8(TEST_INFORMATION), 0, 0x28u);
    *recomp_memory_u32(TEST_INFORMATION + 0x20u) = attributes;
    uint32_t status = invoke(226u, args, 5u, passed);
    *passed &= expect("set-attributes IOSB status",
        *recomp_memory_u32(TEST_IOSB) == status);
    return status;
}

static uint32_t open_for_delete(const char *guest_path,
    uint32_t desired_access, int *passed)
{
    const uint32_t args[] = {TEST_HANDLE, desired_access, TEST_ATTRIBUTES,
        TEST_IOSB, 7u, 0u};
    set_path(guest_path);
    uint32_t status = invoke(202u, args, 6u, passed);
    *passed &= expect("open for delete status", status == 0u &&
        *recomp_memory_u32(TEST_HANDLE) != 0u);
    return *recomp_memory_u32(TEST_HANDLE);
}

static uint32_t query_information(uint32_t handle, uint32_t kind,
    uint32_t length, int *passed)
{
    const uint32_t args[] = {handle, TEST_IOSB, TEST_INFORMATION, length, kind};
    return invoke(211u, args, 5u, passed);
}

static int close_file(uint32_t handle, int *passed)
{
    return expect("close status", invoke(187u, &handle, 1u, passed) == 0u);
}

static int file_equals(const char *path, const char *expected)
{
    char bytes[32];
    DWORD count = 0u;
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int equal;
    if (file == INVALID_HANDLE_VALUE) return 0;
    equal = ReadFile(file, bytes, sizeof bytes, &count, NULL) &&
        count == strlen(expected) && memcmp(bytes, expected, count) == 0;
    CloseHandle(file);
    return equal;
}

int recomp_kernel_file_save_test(void)
{
    static uint8_t memory[TEST_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_BASE, .size = sizeof memory, .data = memory,
    };
    const char *old_root = recomp_disc_root_path;
    const char guest_file[] = "\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat";
    char temporary[MAX_PATH], root[MAX_PATH], live[MAX_PATH], path[MAX_PATH];
    char delete_file_path[MAX_PATH] = {0};
    char transaction_file_path[MAX_PATH] = {0};
    char readonly_file_path[MAX_PATH] = {0};
    char delete_directory_path[MAX_PATH] = {0};
    char directory_child_path[MAX_PATH] = {0};
    char on_close_path[MAX_PATH] = {0};
    char created_path[MAX_PATH] = {0};
    char read_create_path[MAX_PATH] = {0};
    uint32_t status, handle;
    int passed = 1;

    if (GetTempPathA(sizeof temporary, temporary) == 0u ||
        GetTempFileNameA(temporary, "rsv", 0u, root) == 0u ||
        strlen(root) > MAX_PATH - 100u || !DeleteFileA(root) ||
        !CreateDirectoryA(root, NULL)) {
        return expect("temporary storage directory", 0);
    }
    snprintf(live, sizeof live, "%s\\.recomp-storage\\partition1\\UDATA", root);
    snprintf(path, sizeof path, "%s\\profile.dat", live);
    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
    recomp_disc_root_path = root;
    if (!recomp_save_initialize(root)) {
        passed = expect("initialize", 0);
        goto cleanup;
    }

    {
        const uint32_t open_args[] = {TEST_HANDLE, 0u, TEST_ATTRIBUTES, TEST_IOSB, 3u, 0u};
        set_path("\\Device\\Harddisk0\\Partition5");
        passed &= expect("open virtual cache", invoke(202u, open_args, 6u, &passed) == 0u);
        handle = *recomp_memory_u32(TEST_HANDLE);
        uint32_t args[10] = {handle, 0u, 0u, 0u, TEST_IOSB, 0x90020u, 0u, 0u, 0u, 0u};
        passed &= expect("virtual cache dismount", invoke(200u, args, 10u, &passed) == 0u);
        args[5] = 0x90024u;
        passed &= expect("unknown FS control rejected", invoke(200u, args, 10u, &passed) == 0xc0000010u);
        passed &= close_file(handle, &passed);
        args[5] = 0x90020u;
        passed &= expect("closed dismount handle rejected", invoke(200u, args, 10u, &passed) == 0xc0000008u);
    }
    passed &= expect("begin complete write", recomp_save_begin(0u));
    passed &= expect("owner active", recomp_save_active(0u) && recomp_save_pending());
    handle = create_file(guest_file, GENERIC_READ | GENERIC_WRITE, 3u, &status, &passed);
    passed &= expect("create writable file", status == 0u && handle != 0u);
    passed &= expect("open protected handle blocks commit", !recomp_kernel_save_handles_closed(0u));
    passed &= expect("complete write", write_file(handle, "abcdef", 6u, &passed) == 0u);
    passed &= expect("generic write grants attribute updates",
        set_file_attributes(handle, FILE_ATTRIBUTE_ARCHIVE, &passed) == 0u);
    passed &= expect("set position", set_information(handle, 14u, 2u, 8u, &passed) == 0u);
    passed &= expect("write at guest cursor", write_file(handle, "XY", 2u, &passed) == 0u);
    passed &= expect("set EOF", set_information(handle, 20u, 4u, 8u, &passed) == 0u);
    passed &= close_file(handle, &passed);
    passed &= expect("closed handles allow commit", recomp_kernel_save_handles_closed(0u));
    passed &= expect("commit complete writes", recomp_save_end(0u, true));
    passed &= expect("committed bytes and EOF", file_equals(path, "abXY"));
    passed &= expect("commit clears operation", !recomp_save_pending());

    status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA",
        GENERIC_READ, 3u, 0u, 1u, &passed);
    uint32_t reader = *recomp_memory_u32(TEST_HANDLE);
    passed &= expect("open directory before rollback", status == 0u);
    passed &= expect("begin rollback with directory reader", recomp_save_begin(0u));
    handle = create_file(guest_file, GENERIC_WRITE, 1u, &status, &passed);
    passed &= expect("write before directory-reader rollback", status == 0u &&
        write_file(handle, "zz", 2u, &passed) == 0u);
    passed &= close_file(handle, &passed);
    passed &= expect("abort with directory reader", !recomp_save_end(0u, false));
    passed &= expect("directory reader allows full rollback", file_equals(path, "abXY"));
    passed &= close_file(reader, &passed);
    bool ready_after_rollback = recomp_save_begin(0u);
    passed &= expect("rollback leaves store ready", ready_after_rollback);
    if (ready_after_rollback) passed &= expect("commit after rollback", recomp_save_end(0u, true));
    else passed &= expect("recover failed fixture", recomp_save_initialize(root));

    handle = open_for_delete(guest_file, FILE_WRITE_ATTRIBUTES, &passed);
    passed &= expect("begin while unrelated metadata handle exists", recomp_save_begin(7u));
    passed &= close_file(handle, &passed);
    passed &= expect("unrelated metadata close permits commit", recomp_save_end(7u, true));

    for (unsigned api = 0; api < 2u; ++api) {
        for (unsigned directory = 0; directory < 2u; ++directory) {
            const uint32_t rights[] = {DELETE, FILE_WRITE_ATTRIBUTES, GENERIC_WRITE, GENERIC_READ};
            for (unsigned i = 0; i < sizeof rights / sizeof rights[0]; ++i) {
                passed &= expect("begin handle ownership check", recomp_save_begin(0u));
                status = open_existing(directory
                    ? "\\Device\\Harddisk0\\partition1\\UDATA" : guest_file,
                    rights[i], 7u, api, directory, &passed);
                handle = *recomp_memory_u32(TEST_HANDLE);
                passed &= expect("open ownership-check handle", status == 0u && handle != 0u);
                passed &= expect("mutation-capable handle blocks commit before mutation",
                    recomp_kernel_save_handles_closed(0u) == (i == 3u));
                if (rights[i] == DELETE) {
                    passed &= expect("cancel unused deletion", set_information(handle, 13u, 0u, 1u, &passed) == 0u);
                    passed &= expect("cancellation retains save ownership", !recomp_kernel_save_handles_closed(0u));
                }
                passed &= close_file(handle, &passed);
                passed &= expect("close releases save ownership", recomp_kernel_save_handles_closed(0u));
                passed &= expect("metadata close does not require data flush", recomp_save_end(0u, true));
            }
        }
    }

    {
        const char *invalid_paths[] = {
            "\\Device\\Harddisk0\\partition1\\UDATA\\bad.",
            "\\Device\\Harddisk0\\partition1\\UDATA\\bad ",
            "\\Device\\Harddisk0\\partition1\\UDATA\\..\\bad",
            "D:\\bad.", "D:\\bad ", "D:\\..\\bad",
        };
        for (unsigned i = 0; i < sizeof invalid_paths / sizeof invalid_paths[0]; ++i) {
            set_path(invalid_paths[i]);
            const uint32_t args[] = {TEST_HANDLE, GENERIC_READ, TEST_ATTRIBUTES,
                TEST_IOSB, 7u, 0u};
            status = invoke(202u, args, 6u, &passed);
            passed &= expect("malformed absolute open fails", status == 0xc000000du &&
                *recomp_memory_u32(TEST_HANDLE) == 0u);
            if (status == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
            handle = create_file(invalid_paths[i], GENERIC_WRITE, 3u, &status, &passed);
            passed &= expect("malformed absolute create fails", status == 0xc000000du &&
                handle == 0u);
            if (status == 0u) passed &= close_file(handle, &passed);
        }
    }

    passed &= expect("begin optional lookup", recomp_save_begin(0u));
    set_path("\\Device\\Harddisk0\\partition1\\UDATA\\missing.dat");
    {
        const uint32_t args[] = {TEST_HANDLE, GENERIC_READ, TEST_ATTRIBUTES, TEST_IOSB, 3u, 0u};
        status = invoke(202u, args, 6u, &passed);
    }
    passed &= expect("optional read missing", status != 0u && *recomp_memory_u32(TEST_HANDLE) == 0u);
    passed &= expect("optional missing read does not poison", recomp_save_end(0u, true));
    passed &= expect("optional read preserves payload", file_equals(path, "abXY"));

    passed &= expect("begin optional read seek", recomp_save_begin(0u));
    set_path(guest_file);
    {
        const uint32_t args[] = {TEST_HANDLE, GENERIC_READ, TEST_ATTRIBUTES, TEST_IOSB, 3u, 0u};
        status = invoke(202u, args, 6u, &passed);
    }
    handle = *recomp_memory_u32(TEST_HANDLE);
    passed &= expect("optional read open", status == 0u && handle != 0u);
    passed &= expect("optional read position", set_information(handle, 14u, 2u, 8u, &passed) == 0u);
    {
        const uint32_t args[] = {handle, 0u, 0u, 0u, TEST_IOSB, TEST_BUFFER, 2u, 0u};
        status = invoke(219u, args, 8u, &passed);
    }
    passed &= expect("read uses optional handle cursor", status == 0u &&
        *recomp_memory_u32(TEST_IOSB) == 0u && *recomp_memory_u32(TEST_IOSB + 4u) == 2u &&
        memcmp(recomp_memory_i8(TEST_BUFFER), "XY", 2u) == 0);
    passed &= close_file(handle, &passed);
    passed &= expect("optional read seek does not poison", recomp_save_end(0u, true));
    passed &= expect("optional read seek preserves payload", file_equals(path, "abXY"));

    passed &= expect("begin failed required open", recomp_save_begin(0u));
    handle = create_file("\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat\\child.dat",
        GENERIC_WRITE, 3u, &status, &passed);
    passed &= expect("parent-file conflict fails open", status != 0u && handle == 0u);
    passed &= expect("required open failure aborts", !recomp_save_end(0u, true));
    passed &= expect("failed open preserves payload", file_equals(path, "abXY"));

    passed &= expect("begin failed required NtOpenFile", recomp_save_begin(0u));
    handle = create_file(guest_file, GENERIC_READ | GENERIC_WRITE, 1u, &status, &passed);
    passed &= expect("open before required NtOpenFile failure", status == 0u && handle != 0u);
    passed &= expect("write before required NtOpenFile failure", write_file(handle, "zz", 2u, &passed) == 0u);
    passed &= close_file(handle, &passed);
    set_path("\\Device\\Harddisk0\\partition1\\UDATA\\missing.dat");
    {
        const uint32_t args[] = {TEST_HANDLE, GENERIC_WRITE, TEST_ATTRIBUTES, TEST_IOSB, 3u, 0u};
        status = invoke(202u, args, 6u, &passed);
    }
    passed &= expect("required NtOpenFile fails", status != 0u &&
        *recomp_memory_u32(TEST_HANDLE) == 0u && *recomp_memory_u32(TEST_IOSB) == status);
    passed &= expect("required NtOpenFile failure aborts success end", !recomp_save_end(0u, true));
    passed &= expect("required NtOpenFile failure rolls back earlier write", file_equals(path, "abXY"));

    uint32_t profile_root = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA", GENERIC_READ, &passed);
    for (unsigned api = 0; api < 2u; ++api) {
        const uint32_t rights[] = {DELETE, FILE_WRITE_ATTRIBUTES, GENERIC_WRITE, GENERIC_READ};
        const char *invalid[] = {"../profile.dat", "nested/../profile.dat"};
        for (unsigned i = 0; i < sizeof rights / sizeof rights[0]; ++i) {
            const bool mutation = rights[i] != GENERIC_READ;
            for (unsigned j = 0; j < 2u; ++j) {
                passed &= expect("begin rejected relative open", recomp_save_begin(0u));
                if (mutation) {
                    handle = create_file(guest_file, GENERIC_WRITE, 1u, &status, &passed);
                    passed &= expect("write before relative open rejection", status == 0u &&
                        write_file(handle, "zz", 2u, &passed) == 0u);
                    passed &= close_file(handle, &passed);
                }
                set_path(invalid[j]);
                *recomp_memory_u32(TEST_ATTRIBUTES) = profile_root;
                const uint32_t open_args[] = {TEST_HANDLE, rights[i], TEST_ATTRIBUTES,
                    TEST_IOSB, 7u, 0u};
                const uint32_t create_args[] = {TEST_HANDLE, rights[i], TEST_ATTRIBUTES,
                    TEST_IOSB, 0u, 0u, 7u, 1u, 0u};
                status = api == 0u ? invoke(202u, open_args, 6u, &passed)
                                   : invoke(190u, create_args, 9u, &passed);
                passed &= expect("relative validation returns failure without a handle",
                    status == 0xc000000du && *recomp_memory_u32(TEST_IOSB) == status &&
                    *recomp_memory_u32(TEST_HANDLE) == 0u);
                passed &= expect("relative mutation failure aborts; optional read does not",
                    recomp_save_end(0u, true) == !mutation);
                passed &= expect("relative rejection preserves earlier payload", file_equals(path, "abXY"));
                /* Keep cases independent if the failed operation wrongly committed. */
                handle = create_file(guest_file, GENERIC_WRITE, 1u, &status, &passed);
                passed &= expect("restore relative-open fixture", status == 0u &&
                    write_file(handle, "abXY", 4u, &passed) == 0u);
                passed &= close_file(handle, &passed);
            }
        }
    }
    passed &= close_file(profile_root, &passed);

    for (unsigned api = 0; api < 2u; ++api) {
        const uint32_t rights[] = {DELETE, FILE_WRITE_ATTRIBUTES,
            DELETE | READ_CONTROL | SYNCHRONIZE,
            FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | SYNCHRONIZE};
        for (unsigned i = 0; i < sizeof rights / sizeof rights[0]; ++i) {
            passed &= expect("begin compatible metadata open", recomp_save_begin(0u));
            HANDLE writer = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            passed &= expect("open writer without read sharing", writer != INVALID_HANDLE_VALUE);
            status = open_existing(guest_file, rights[i], FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                api, 0u, &passed);
            passed &= expect("metadata open does not request data reads", status == 0u);
            if (status == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            passed &= expect("compatible metadata open permits commit", recomp_save_end(0u, true));
        }
        status = open_existing(guest_file, DELETE, FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            api, 0u, &passed);
        handle = *recomp_memory_u32(TEST_HANDLE);
        passed &= expect("open delete handle denying read sharing", status == 0u);
        {
            const uint32_t outer_handle = handle;
            uint32_t reader = open_existing(guest_file, GENERIC_READ, 7u, api, 0u, &passed);
            passed &= expect("delete handle preserves guest read-sharing denial",
                reader == 0xc0000043u && *recomp_memory_u32(TEST_HANDLE) == 0u);
            if (reader == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
            handle = outer_handle;
        }
        if (status == 0u) passed &= close_file(handle, &passed);

        status = open_existing(guest_file, DELETE | GENERIC_READ, 7u, api, 0u, &passed);
        handle = *recomp_memory_u32(TEST_HANDLE);
        passed &= expect("open metadata handle with requested read access", status == 0u);
        const uint32_t read_args[] = {handle, 0u, 0u, 0u, TEST_IOSB, TEST_BUFFER, 4u, 0u};
        passed &= expect("explicit metadata read access preserved",
            invoke(219u, read_args, 8u, &passed) == 0u &&
            memcmp(recomp_memory_i8(TEST_BUFFER), "abXY", 4u) == 0);
        if (status == 0u) passed &= close_file(handle, &passed);

        status = open_existing(guest_file, DELETE | FILE_WRITE_DATA, 7u, api, 0u, &passed);
        handle = *recomp_memory_u32(TEST_HANDLE);
        passed &= expect("open metadata handle without untracked write rights", status == 0u);
        passed &= expect("metadata mask does not grant untracked data writes",
            write_file(handle, "lost", 4u, &passed) != 0u);
        if (status == 0u) passed &= close_file(handle, &passed);
        passed &= expect("denied untracked write preserves payload", file_equals(path, "abXY"));

        status = open_existing(guest_file, GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            api, 0u, &passed);
        handle = *recomp_memory_u32(TEST_HANDLE);
        passed &= expect("open reader denying read sharing", status == 0u);
        passed &= expect("exclusive guest reader does not block the snapshot",
            recomp_save_begin(0u));
        {
            const uint32_t outer_handle = handle;
            uint32_t second = open_existing(guest_file, GENERIC_READ, 7u, api, 0u, &passed);
            passed &= expect("exclusive guest reader blocks other guest readers",
                second == 0xc0000043u);
            if (second == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
            handle = outer_handle;
        }
        passed &= expect("save commits beside an exclusive reader", recomp_save_end(0u, true));
        if (status == 0u) passed &= close_file(handle, &passed);
        passed &= expect("closed reader permits reinitialization", recomp_save_initialize(root));
        passed &= expect("failed snapshot leaves payload intact", file_equals(path, "abXY"));
    }

    for (unsigned api = 0; api < 2u; ++api) {
        const uint32_t rights[] = {DELETE, FILE_WRITE_ATTRIBUTES};
        for (unsigned i = 0; i < 2u; ++i) {
            passed &= expect("begin required metadata open", recomp_save_begin(0u));
            handle = create_file(guest_file, GENERIC_READ | GENERIC_WRITE, 1u, &status, &passed);
            passed &= expect("write before metadata open failure",
                status == 0u && write_file(handle, "zz", 2u, &passed) == 0u);
            passed &= close_file(handle, &passed);
            HANDLE blocker = CreateFileA(path, GENERIC_READ, 0, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            passed &= expect("open native sharing blocker",
                blocker != INVALID_HANDLE_VALUE);
            /* Attribute access alone is exempt from native sharing restrictions. */
            set_path(i == 0u ? guest_file : "\\Device\\Harddisk0\\partition1\\UDATA\\missing.dat");
            const uint32_t open_args[] = {TEST_HANDLE, rights[i], TEST_ATTRIBUTES,
                TEST_IOSB, 7u, 0u};
            const uint32_t create_args[] = {TEST_HANDLE, rights[i], TEST_ATTRIBUTES,
                TEST_IOSB, 0u, 0u, 7u, 1u, 0u};
            status = api == 0u ? invoke(202u, open_args, 6u, &passed)
                               : invoke(190u, create_args, 9u, &passed);
            passed &= expect("metadata open failure returned", status != 0u);
            if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
            passed &= expect("metadata open failure aborts success end",
                !recomp_save_end(0u, true));
            passed &= expect("metadata open failure rolls back earlier write",
                file_equals(path, "abXY"));
        }
    }
    {
        /* A blocked existing file must not become a placeholder handle. */
        passed &= expect("begin blocked open-if delete", recomp_save_begin(0u));
        HANDLE blocker = CreateFileA(path, GENERIC_READ, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        const uint32_t args[] = {TEST_HANDLE, DELETE, TEST_ATTRIBUTES,
            TEST_IOSB, 0u, 0u, 7u, 3u, 0u};
        set_path(guest_file);
        status = invoke(190u, args, 9u, &passed);
        passed &= expect("blocked open-if delete reports sharing violation",
            status == 0xc0000043u);
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        passed &= expect("blocked open-if delete aborts save", !recomp_save_end(0u, true));
        passed &= expect("blocked open-if delete keeps file", file_equals(path, "abXY"));
    }

    passed &= expect("begin failed write", recomp_save_begin(0u));
    handle = create_file(guest_file, GENERIC_READ | GENERIC_WRITE, 1u, &status, &passed);
    passed &= expect("open for locked write", status == 0u && handle != 0u);
    {
        HANDLE blocker = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        int locked = blocker != INVALID_HANDLE_VALUE && LockFile(blocker, 0u, 0u, 4u, 0u);
        passed &= expect("real Win32 byte-range lock", locked);
        if (locked) {
            passed &= expect("locked write fails", write_file(handle, "NOPE", 4u, &passed) != 0u);
            UnlockFile(blocker, 0u, 0u, 4u, 0u);
        } else {
            recomp_save_note_failure(0u);
        }
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
    }
    passed &= close_file(handle, &passed);
    passed &= expect("failed write aborts", !recomp_save_end(0u, true));
    passed &= expect("failed write restores payload", file_equals(path, "abXY"));

    passed &= expect("begin failed SetInfo", recomp_save_begin(0u));
    handle = create_file(guest_file, GENERIC_READ | GENERIC_WRITE, 1u, &status, &passed);
    passed &= expect("open for SetInfo", status == 0u && handle != 0u);
    passed &= expect("write before failed SetInfo", write_file(handle, "zz", 2u, &passed) == 0u);
    passed &= expect("short position structure fails", set_information(handle, 14u, 0u, 4u, &passed) != 0u);
    passed &= close_file(handle, &passed);
    passed &= expect("SetInfo failure aborts", !recomp_save_end(0u, true));
    passed &= expect("SetInfo failure rolls back earlier write", file_equals(path, "abXY"));

    /* An old synthetic handle must not acknowledge a protected write. */
    handle = create_file("D:\\metadata.xbx", GENERIC_WRITE, 3u, &status, &passed);
    passed &= expect("create legacy pseudo handle", status == 0u && handle != 0u);
    passed &= expect("begin pseudo-write rejection", recomp_save_begin(0u));
    passed &= expect("active pseudo write fails", write_file(handle, "lost", 4u, &passed) != 0u);
    passed &= close_file(handle, &passed);
    passed &= expect("pseudo write failure aborts", !recomp_save_end(0u, true));
    passed &= expect("pseudo failure preserves payload", file_equals(path, "abXY"));

    snprintf(delete_file_path, sizeof delete_file_path,
        "%s\\delete.dat", live);
    HANDLE deletion_file = CreateFileA(delete_file_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    passed &= expect("create file for disposition",
        deletion_file != INVALID_HANDLE_VALUE);
    if (deletion_file != INVALID_HANDLE_VALUE) CloseHandle(deletion_file);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete.dat", GENERIC_READ, &passed);
    passed &= expect("disposition requires delete access",
        set_information(handle, 13u, 1u, 1u, &passed) == 0xc0000022u);
    passed &= expect("zero attributes still require attribute access",
        set_file_attributes(handle, 0u, &passed) == 0xc0000022u);
    passed &= close_file(handle, &passed);
    passed &= expect("access denial retains file",
        GetFileAttributesA(delete_file_path) != INVALID_FILE_ATTRIBUTES);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete.dat", 0x10000000u, &passed);
    passed &= expect("GENERIC_ALL grants delete access",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u &&
        set_information(handle, 13u, 0u, 1u, &passed) == 0u);
    passed &= close_file(handle, &passed);
    {
        snprintf(on_close_path, sizeof on_close_path, "%s\\on-close.dat", live);
        for (unsigned api = 0; api < 2u; ++api) {
            HANDLE created = CreateFileA(on_close_path, GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            passed &= expect("create delete-on-close file", created != INVALID_HANDLE_VALUE);
            if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
            status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\on-close.dat",
                GENERIC_READ, 7u, api, 0x1000u, &passed);
            passed &= expect("delete-on-close requires delete access",
                status == 0xc000000du && *recomp_memory_u32(TEST_HANDLE) == 0u);
            status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\on-close.dat",
                DELETE, 7u, api, 0x1000u, &passed);
            handle = *recomp_memory_u32(TEST_HANDLE);
            passed &= expect("open with delete-on-close", status == 0u && handle != 0u);
            passed &= expect("delete-on-close file remains until close",
                GetFileAttributesA(on_close_path) != INVALID_FILE_ATTRIBUTES);
            passed &= close_file(handle, &passed);
            passed &= expect("delete-on-close removes file on close",
                GetFileAttributesA(on_close_path) == INVALID_FILE_ATTRIBUTES);
        }
        HANDLE kept = CreateFileA(on_close_path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0u;
        passed &= expect("create file for rejected overwrite",
            kept != INVALID_HANDLE_VALUE &&
            WriteFile(kept, "keep", 4u, &written, NULL) != 0 && written == 4u);
        if (kept != INVALID_HANDLE_VALUE) CloseHandle(kept);
        const uint32_t overwrite_args[] = {TEST_HANDLE, GENERIC_WRITE, TEST_ATTRIBUTES,
            TEST_IOSB, 0u, 0u, 7u, 5u, 0x1000u};
        set_path("\\Device\\Harddisk0\\partition1\\UDATA\\on-close.dat");
        status = invoke(190u, overwrite_args, 9u, &passed);
        passed &= expect("delete-on-close overwrite without delete access rejected",
            status == 0xc000000du && *recomp_memory_u32(TEST_HANDLE) == 0u);
        passed &= expect("rejected overwrite keeps contents", file_equals(on_close_path, "keep"));
    }
    {
        const uint32_t args[] = {TEST_HANDLE, FILE_WRITE_ATTRIBUTES, TEST_ATTRIBUTES,
            TEST_IOSB, 0u, 0u, 7u, 3u, 0u};
        snprintf(created_path, sizeof created_path, "%s\\metadata-create.dat", live);
        passed &= expect("begin metadata-only create", recomp_save_begin(0u));
        set_path("\\Device\\Harddisk0\\partition1\\UDATA\\metadata-create.dat");
        status = invoke(190u, args, 9u, &passed);
        handle = *recomp_memory_u32(TEST_HANDLE);
        passed &= expect("metadata-only create makes a real file", status == 0u &&
            GetFileAttributesA(created_path) != INVALID_FILE_ATTRIBUTES);
        passed &= expect("metadata-only create sets attributes",
            set_file_attributes(handle, FILE_ATTRIBUTE_HIDDEN, &passed) == 0u &&
            (GetFileAttributesA(created_path) & FILE_ATTRIBUTE_HIDDEN) != 0u);
        passed &= expect("basic query reports updated attributes",
            query_information(handle, 4u, 0x28u, &passed) == 0u &&
            (*recomp_memory_u32(TEST_INFORMATION + 0x20u) & FILE_ATTRIBUTE_HIDDEN) != 0u &&
            *recomp_memory_u32(TEST_INFORMATION + 0x1cu) != 0u);
        passed &= expect("basic query rejects the unpadded length",
            query_information(handle, 4u, 0x24u, &passed) == 0xc0000004u);
        passed &= expect("network query reports updated attributes",
            query_information(handle, 0x22u, 0x38u, &passed) == 0u &&
            (*recomp_memory_u32(TEST_INFORMATION + 0x30u) & FILE_ATTRIBUTE_HIDDEN) != 0u);
        {
            const uint32_t collide_args[] = {TEST_HANDLE, DELETE, TEST_ATTRIBUTES,
                TEST_IOSB, 0u, 0u, 7u, 2u, 0u};
            status = invoke(190u, collide_args, 9u, &passed);
            passed &= expect("metadata-only FILE_CREATE on existing file collides",
                status == 0xc0000035u && *recomp_memory_u32(TEST_HANDLE) == 0u);
        }
        passed &= close_file(handle, &passed);
        passed &= expect("abandon metadata-only create", !recomp_save_end(0u, false));
        passed &= expect("rollback removes metadata-only create",
            GetFileAttributesA(created_path) == INVALID_FILE_ATTRIBUTES);
    }
    {
        /* A read-only create is still a mutation; releasing native handles
           lets rollback remove the file while the guest handle stays open. */
        snprintf(read_create_path, sizeof read_create_path, "%s\\read-create.dat", live);
        passed &= expect("begin read-only create", recomp_save_begin(0u));
        handle = create_file("\\Device\\Harddisk0\\partition1\\UDATA\\read-create.dat", GENERIC_READ, 2u, &status, &passed);
        passed &= expect("read-only create succeeds", status == 0u && handle != 0u);
        passed &= expect("read-only create blocks commit", !recomp_kernel_save_handles_closed(0u));
        recomp_kernel_release_profile_handles();
        passed &= expect("rollback with released handle", !recomp_save_end(0u, false));
        passed &= expect("rollback removes read-only create",
            GetFileAttributesA(read_create_path) == INVALID_FILE_ATTRIBUTES);
        passed &= close_file(handle, &passed);

        passed &= expect("begin invalid-handle mutation", recomp_save_begin(0u));
        passed &= expect("disposition on invalid handle fails",
            set_information(0x7ffffff0u, 13u, 1u, 1u, &passed) == 0xc0000008u);
        passed &= expect("invalid-handle mutation aborts save", !recomp_save_end(0u, true));

        handle = open_for_delete("\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat", DELETE | GENERIC_READ, &passed);
        passed &= expect("set file position", set_information(handle, 14u, 5u, 8u, &passed) == 0u);
        passed &= expect("position query reports cursor",
            query_information(handle, 14u, 8u, &passed) == 0u &&
            *recomp_memory_u32(TEST_INFORMATION) == 5u);
        passed &= expect("begin rename", recomp_save_begin(0u));
        passed &= expect("profile rename unsupported",
            set_information(handle, 10u, 0u, 0x10u, &passed) == 0xc00000bbu);
        passed &= expect("rename aborts save", recomp_save_end_recovers(0u, true));
        recomp_kernel_release_profile_handles();
        passed &= expect("rejected rename rolls back", !recomp_save_end(0u, true) &&
            file_equals(path, "abXY"));
        passed &= close_file(handle, &passed);

        handle = open_for_delete("\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat", FILE_WRITE_ATTRIBUTES, &passed);
        memset(recomp_memory_i8(TEST_INFORMATION), 0, 0x28u);
        *recomp_memory_u32(TEST_INFORMATION + 0x10u) = 0x12345678u;
        *recomp_memory_u32(TEST_INFORMATION + 0x14u) = 0x01d00000u;
        {
            const uint32_t args[] = {handle, TEST_IOSB, TEST_INFORMATION, 0x28u, 4u};
            passed &= expect("set write time", invoke(226u, args, 5u, &passed) == 0u);
        }
        WIN32_FILE_ATTRIBUTE_DATA timed;
        passed &= expect("write time applied",
            GetFileAttributesExA(path, GetFileExInfoStandard, &timed) != 0 &&
            timed.ftLastWriteTime.dwLowDateTime == 0x12345678u &&
            timed.ftLastWriteTime.dwHighDateTime == 0x01d00000u);
        passed &= close_file(handle, &passed);

        status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat", GENERIC_READ, 7u, 1u, 1u, &passed);
        passed &= expect("folder open on a file reports not a directory",
            status == 0xc0000103u && *recomp_memory_u32(TEST_HANDLE) == 0u);

        HANDLE blocker = CreateFileA(path, GENERIC_READ, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\profile.dat", GENERIC_WRITE, 7u, 0u, 0u, &passed);
        passed &= expect("blocked write open reports sharing violation",
            status == 0xc0000043u && *recomp_memory_u32(TEST_HANDLE) == 0u);
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
    }
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete.dat",
        0x00110000u, &passed);
    passed &= expect("set file disposition",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= expect("file remains until close",
        GetFileAttributesA(delete_file_path) != INVALID_FILE_ATTRIBUTES);
    {
        set_path("\\Device\\Harddisk0\\partition1\\UDATA\\DELETE.DAT");
        const uint32_t args[] = {TEST_HANDLE, GENERIC_READ, TEST_ATTRIBUTES,
            TEST_IOSB, 7u, 0u};
        status = invoke(202u, args, 6u, &passed);
        passed &= expect("pending file blocks case-insensitive reopen", status == 0xc0000056u);
        if (status == 0u) close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
        const uint32_t create_args[] = {TEST_HANDLE, GENERIC_WRITE, TEST_ATTRIBUTES,
            TEST_IOSB, 0u, 0u, 7u, 5u, 0u};
        status = invoke(190u, create_args, 9u, &passed);
        passed &= expect("pending file blocks overwrite", status == 0xc0000056u);
        if (status == 0u) close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
        passed &= expect("cancel pending disposition",
            set_information(handle, 13u, 0u, 1u, &passed) == 0u);
        uint32_t reopened = open_for_delete(
            "\\Device\\Harddisk0\\partition1\\UDATA\\delete.dat", GENERIC_READ, &passed);
        passed &= close_file(reopened, &passed);
        passed &= expect("restore pending disposition",
            set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    }
    passed &= close_file(handle, &passed);
    passed &= expect("file removed on close",
        GetFileAttributesA(delete_file_path) == INVALID_FILE_ATTRIBUTES);

    snprintf(transaction_file_path, sizeof transaction_file_path,
        "%s\\transaction-delete.dat", live);
    deletion_file = CreateFileA(transaction_file_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    passed &= expect("create transactional delete file",
        deletion_file != INVALID_HANDLE_VALUE);
    if (deletion_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0u;
        passed &= expect("write transactional delete file",
            WriteFile(deletion_file, "before", 6u, &written, NULL) != 0 &&
                written == 6u);
        CloseHandle(deletion_file);
    }
    DWORD transaction_file_attributes = GetFileAttributesA(
        transaction_file_path);
    passed &= expect("query transactional delete attributes",
        transaction_file_attributes != INVALID_FILE_ATTRIBUTES);
    passed &= expect("begin delete transaction", recomp_save_begin(0u));
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\transaction-delete.dat",
        0x00110100u, &passed);
    passed &= expect("set attributes inside transaction",
        set_file_attributes(handle, FILE_ATTRIBUTE_READONLY, &passed) == 0u);
    passed &= expect("transaction applies attributes",
        (GetFileAttributesA(transaction_file_path) & FILE_ATTRIBUTE_READONLY) != 0u);
    passed &= expect("clear attributes inside transaction",
        set_file_attributes(handle, FILE_ATTRIBUTE_NORMAL, &passed) == 0u);
    passed &= expect("set transactional disposition",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= expect("pending deletion blocks transaction end",
        !recomp_kernel_save_handles_closed(0u));
    passed &= close_file(handle, &passed);
    passed &= expect("closed deletion handle allows transaction end",
        recomp_kernel_save_handles_closed(0u));
    passed &= expect("transactional delete takes effect",
        GetFileAttributesA(transaction_file_path) == INVALID_FILE_ATTRIBUTES);
    passed &= expect("rollback restores transactional delete",
        !recomp_save_end(0u, false) &&
            file_equals(transaction_file_path, "before") &&
            GetFileAttributesA(transaction_file_path) == transaction_file_attributes);

    for (unsigned information_class = 0; information_class < 2u; ++information_class) {
        handle = open_for_delete(
            "\\Device\\Harddisk0\\partition1\\UDATA\\transaction-delete.dat",
            FILE_WRITE_ATTRIBUTES, &passed);
        passed &= expect("begin foreign-owner metadata rejection", recomp_save_begin(7u));
        status = information_class == 0u
            ? set_file_attributes(handle, FILE_ATTRIBUTE_READONLY, &passed)
            : set_information(handle, 13u, 1u, 1u, &passed);
        passed &= expect("foreign owner metadata update rejected", status == 0xc0000001u);
        passed &= close_file(handle, &passed);
        passed &= expect("rejected metadata update poisons pending owner",
            !recomp_save_end(7u, true));
        passed &= expect("rejected metadata update preserves file",
            file_equals(transaction_file_path, "before") &&
            GetFileAttributesA(transaction_file_path) == transaction_file_attributes);
    }

    passed &= expect("begin cross-owner delete close", recomp_save_begin(0u));
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\transaction-delete.dat",
        DELETE, &passed);
    passed &= expect("mark owner zero delete pending",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= expect("other owner close is rejected",
        recomp_kernel_close_file(handle, 7u) != 0u);
    passed &= expect("rejected close poisons the delete owner",
        !recomp_save_end(0u, true));
    passed &= expect("rejected close preserves payload", file_equals(transaction_file_path, "before"));

    snprintf(readonly_file_path, sizeof readonly_file_path,
        "%s\\readonly.dat", live);
    deletion_file = CreateFileA(readonly_file_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    passed &= expect("create read-only file for disposition",
        deletion_file != INVALID_HANDLE_VALUE);
    if (deletion_file != INVALID_HANDLE_VALUE) CloseHandle(deletion_file);
    passed &= expect("mark host file read-only",
        SetFileAttributesA(readonly_file_path, FILE_ATTRIBUTE_READONLY) != 0);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\readonly.dat",
        0x00110100u, &passed);
    passed &= expect("read-only disposition rejected",
        set_information(handle, 13u, 1u, 1u, &passed) == 0xc0000121u);
    passed &= expect("read-only file retained",
        GetFileAttributesA(readonly_file_path) != INVALID_FILE_ATTRIBUTES);
    passed &= expect("clear read-only attribute for delete",
        set_file_attributes(handle, FILE_ATTRIBUTE_NORMAL, &passed) == 0u);
    passed &= expect("set read-only file disposition",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= close_file(handle, &passed);
    passed &= expect("read-only file removed on close",
        GetFileAttributesA(readonly_file_path) == INVALID_FILE_ATTRIBUTES);

    snprintf(delete_directory_path, sizeof delete_directory_path,
        "%s\\delete-directory", live);
    passed &= expect("create directory for disposition",
        CreateDirectoryA(delete_directory_path, NULL) != 0);
    for (unsigned first_api = 0; first_api < 2u; ++first_api) {
        for (unsigned second_api = 0; second_api < 2u; ++second_api) {
            for (unsigned reverse = 0; reverse < 2u; ++reverse) {
                status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
                    reverse ? DELETE : GENERIC_READ, reverse ? 7u : 3u, first_api, 1u, &passed);
                handle = *recomp_memory_u32(TEST_HANDLE);
                passed &= expect("open first shared directory", status == 0u && handle != 0u);
                status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\DELETE-DIRECTORY",
                    reverse ? GENERIC_READ : DELETE, reverse ? 3u : 7u, second_api, 1u, &passed);
                passed &= expect("directory delete sharing is symmetric", status == 0xc0000043u &&
                    *recomp_memory_u32(TEST_HANDLE) == 0u);
                if (status == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
                passed &= close_file(handle, &passed);
                status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
                    reverse ? GENERIC_READ : DELETE, reverse ? 3u : 7u, second_api, 1u, &passed);
                passed &= expect("directory close releases sharing constraint", status == 0u);
                if (status == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
            }
        }
    }
    status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
        GENERIC_READ, 1u, 0u, 1u, &passed);
    handle = *recomp_memory_u32(TEST_HANDLE);
    passed &= expect("open directory denying write sharing", status == 0u && handle != 0u);
    passed &= expect("directory query reports directory attributes",
        query_information(handle, 4u, 0x28u, &passed) == 0u &&
        (*recomp_memory_u32(TEST_INFORMATION + 0x20u) & FILE_ATTRIBUTE_DIRECTORY) != 0u);
    status = open_existing("\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
        GENERIC_WRITE, 7u, 1u, 1u, &passed);
    passed &= expect("directory write sharing enforced", status == 0xc0000043u &&
        *recomp_memory_u32(TEST_HANDLE) == 0u);
    if (status == 0u) passed &= close_file(*recomp_memory_u32(TEST_HANDLE), &passed);
    passed &= close_file(handle, &passed);
    snprintf(directory_child_path, sizeof directory_child_path,
        "%s\\child.dat", delete_directory_path);
    deletion_file = CreateFileA(directory_child_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    passed &= expect("create child in directory for disposition",
        deletion_file != INVALID_HANDLE_VALUE);
    if (deletion_file != INVALID_HANDLE_VALUE) CloseHandle(deletion_file);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
        0x00110000u, &passed);
    passed &= expect("non-empty directory disposition rejected",
        set_information(handle, 13u, 1u, 1u, &passed) == 0xc0000101u);
    {
        uint32_t args[] = {handle, 0u, 0u, 0u, TEST_IOSB,
            TEST_BUFFER, 0x100u, 1u, 0u, 0x12345601u};
        passed &= expect("enumerate child with byte restart flag",
            invoke(207u, args, 10u, &passed) == 0u &&
            strcmp((char *)recomp_memory_i8(TEST_BUFFER + 0x40u), "child.dat") == 0);
        args[9] = 0x12345600u;
        passed &= expect("scan ignores upper bits of false restart flag",
            invoke(207u, args, 10u, &passed) == 0x80000006u);
        set_path("child.dat");
        *recomp_memory_u32(TEST_ATTRIBUTES) = handle;
        const uint32_t open_args[] = {TEST_HANDLE, 0x00110100u,
            TEST_ATTRIBUTES, TEST_IOSB, 7u, 0x4040u};
        passed &= expect("open child relative to directory handle",
            invoke(202u, open_args, 6u, &passed) == 0u);
        uint32_t child = *recomp_memory_u32(TEST_HANDLE);
        const struct {
            uint32_t root;
            const char *name;
            uint32_t status;
        } rejected[] = {
            {0xdeadbeefu, "child.dat", 0xc0000008u},
            {child, "child.dat", 0xc0000008u},
            {handle, "../profile.dat", 0xc000000du},
            {handle, ".. /profile.dat", 0xc000000du},
        };
        for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
            set_path(rejected[i].name);
            *recomp_memory_u32(TEST_ATTRIBUTES) = rejected[i].root;
            passed &= expect("invalid relative open rejected",
                invoke(202u, open_args, 6u, &passed) == rejected[i].status &&
                *recomp_memory_u32(TEST_HANDLE) == 0u);
            const uint32_t create_args[] = {TEST_HANDLE, GENERIC_WRITE,
                TEST_ATTRIBUTES, TEST_IOSB, 0u, 0u, 7u, 3u, 0u};
            passed &= expect("invalid relative create does not become pseudo success",
                invoke(190u, create_args, 9u, &passed) == rejected[i].status &&
                *recomp_memory_u32(TEST_HANDLE) == 0u);
        }
        passed &= expect("set relative child disposition",
            set_information(child, 13u, 1u, 1u, &passed) == 0u);
        passed &= close_file(child, &passed);
        passed &= expect("relative child removed on close",
            GetFileAttributesA(directory_child_path) == INVALID_FILE_ATTRIBUTES);
    }
    passed &= close_file(handle, &passed);
    passed &= expect("non-empty directory retained",
        GetFileAttributesA(delete_directory_path) != INVALID_FILE_ATTRIBUTES);
    passed &= expect("mark directory read-only",
        SetFileAttributesA(delete_directory_path, FILE_ATTRIBUTE_READONLY) != 0);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
        0x00110000u, &passed);
    passed &= expect("read-only directory disposition rejected",
        set_information(handle, 13u, 1u, 1u, &passed) == 0xc0000121u);
    passed &= close_file(handle, &passed);
    passed &= expect("read-only directory retained",
        GetFileAttributesA(delete_directory_path) != INVALID_FILE_ATTRIBUTES);
    passed &= expect("clear directory read-only attribute",
        SetFileAttributesA(delete_directory_path, FILE_ATTRIBUTE_NORMAL) != 0);
    handle = open_for_delete(
        "\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory",
        0x00110000u, &passed);
    passed &= expect("set directory disposition",
        set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= expect("directory remains until close",
        GetFileAttributesA(delete_directory_path) != INVALID_FILE_ATTRIBUTES);
    {
        set_path("child.dat");
        *recomp_memory_u32(TEST_ATTRIBUTES) = handle;
        const uint32_t args[] = {TEST_HANDLE, GENERIC_WRITE, TEST_ATTRIBUTES,
            TEST_IOSB, 0u, 0u, 7u, 3u, 1u};
        passed &= expect("pending directory blocks relative child creation",
            invoke(190u, args, 9u, &passed) == 0xc0000056u &&
            *recomp_memory_u32(TEST_HANDLE) == 0u);
        passed &= expect("pending directory was not repopulated",
            GetFileAttributesA(directory_child_path) == INVALID_FILE_ATTRIBUTES);
    }
    passed &= close_file(handle, &passed);
    passed &= expect("directory removed on close",
        GetFileAttributesA(delete_directory_path) == INVALID_FILE_ATTRIBUTES);

    passed &= expect("begin real close failure", recomp_save_begin(0u));
    handle = open_for_delete("\\Device\\Harddisk0\\partition1\\UDATA\\transaction-delete.dat",
        DELETE, &passed);
    passed &= expect("mark delete before host rejection", set_information(handle, 13u, 1u, 1u, &passed) == 0u);
    passed &= expect("make pending delete read-only",
        SetFileAttributesA(transaction_file_path, FILE_ATTRIBUTE_READONLY) != 0);
    passed &= expect("real delete error still fails close", invoke(187u, &handle, 1u, &passed) != 0u);
    passed &= expect("real delete error aborts save", !recomp_save_end(0u, true));
    passed &= expect("real delete error restores original file", file_equals(transaction_file_path, "before"));

    for (unsigned directory = 0; directory < 2u; ++directory) {
        const char *guest_path = directory
            ? "\\Device\\Harddisk0\\partition1\\UDATA\\delete-directory"
            : "\\Device\\Harddisk0\\partition1\\UDATA\\transaction-delete.dat";
        const char *host_path = directory ? delete_directory_path : transaction_file_path;
        if (directory) passed &= expect("create duplicate-delete directory", CreateDirectoryA(host_path, NULL) != 0);
        for (unsigned commit = 0; commit < 2u; ++commit) {
            uint32_t handles[2];
            passed &= expect("begin duplicate delete", recomp_save_begin(0u));
            for (unsigned api = 0; api < 2u; ++api) {
                status = open_existing(guest_path, DELETE, 7u, api, directory, &passed);
                handles[api] = *recomp_memory_u32(TEST_HANDLE);
                passed &= expect("open duplicate delete handle", status == 0u);
            }
            for (unsigned i = 0; i < 2u; ++i)
                passed &= expect("mark duplicate delete", set_information(handles[i], 13u, 1u, 1u, &passed) == 0u);
            passed &= close_file(handles[0], &passed);
            passed &= expect("remaining delete handle blocks commit", !recomp_kernel_save_handles_closed(0u));
            passed &= close_file(handles[1], &passed);
            passed &= expect("duplicate delete handles released", recomp_kernel_save_handles_closed(0u));
            passed &= expect("duplicate delete preserves transaction outcome", recomp_save_end(0u, commit != 0u) == (commit != 0u));
            passed &= expect("duplicate delete rollback or commit preserved",
                (GetFileAttributesA(host_path) == INVALID_FILE_ATTRIBUTES) == (commit != 0u));
        }
    }

cleanup:
    /* Reinitialization releases the documented lifetime journal lock. A null
       root leaves the backend disabled after this fixture. */
    passed &= expect("release backend ownership", !recomp_save_initialize(NULL));
    recomp_disc_root_path = old_root;
    if (readonly_file_path[0] != '\0') {
        SetFileAttributesA(readonly_file_path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(readonly_file_path);
    }
    if (delete_file_path[0] != '\0') DeleteFileA(delete_file_path);
    if (transaction_file_path[0] != '\0') DeleteFileA(transaction_file_path);
    if (on_close_path[0] != '\0') DeleteFileA(on_close_path);
    if (read_create_path[0] != '\0') DeleteFileA(read_create_path);
    if (created_path[0] != '\0') {
        SetFileAttributesA(created_path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(created_path);
    }
    if (delete_directory_path[0] != '\0') {
        if (directory_child_path[0] != '\0') DeleteFileA(directory_child_path);
        RemoveDirectoryA(delete_directory_path);
    }
    DeleteFileA(path);
    RemoveDirectoryA(live);
    snprintf(path, sizeof path, "%s\\.recomp-storage\\partition1", root);
    RemoveDirectoryA(path);
    snprintf(path, sizeof path, "%s\\.recomp-storage\\save-undo-v1\\version", root);
    DeleteFileA(path);
    snprintf(path, sizeof path, "%s\\.recomp-storage\\save-undo-v1\\lock", root);
    DeleteFileA(path);
    snprintf(path, sizeof path, "%s\\.recomp-storage\\save-undo-v1", root);
    RemoveDirectoryA(path);
    snprintf(path, sizeof path, "%s\\.recomp-storage", root);
    RemoveDirectoryA(path);
    passed &= expect("temporary storage removed", RemoveDirectoryA(root));
    return passed;
}
