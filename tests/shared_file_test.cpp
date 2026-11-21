#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#endif

#include "platform/process.h"
#include "platform/shared_file.h"
#include "scratch_dir.h"

#if !defined(_WIN32)
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace hailstone;
namespace fs = std::filesystem;

namespace {
std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string mapped_text(const platform::ReadOnlyMapping& mapping) {
    const auto data = mapping.bytes();
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

fs::path private_cache(const test::ScratchDir& dir) {
    // macOS's system temp path may itself start with the /var -> /private/var alias.
    return fs::canonical(dir.path()) / "cache";
}

std::size_t temporary_count(const fs::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with(".tmp-")) ++count;
    return count;
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(static_cast<bool>(out));
}

#if defined(_WIN32)
// Creates the directory `path` with a protected DACL granting the current user and each of `others` full access.
// The descriptor goes on at creation, as the client's own: Wine keeps it then, but reports a DACL set later from
// the directory's Unix mode instead.
bool create_with_dacl(const fs::path& path, std::initializer_list<WELL_KNOWN_SID_TYPE> others) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> user(size);
    const bool got = size != 0 && ::GetTokenInformation(token, TokenUser, user.data(), size, &size);
    ::CloseHandle(token);
    if (!got) return false;
    const PSID user_sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    std::vector<std::vector<std::byte>> sids;
    std::vector<EXPLICIT_ACCESS_W> entries;
    auto grant = [&](PSID sid) {
        EXPLICIT_ACCESS_W entry{};
        entry.grfAccessPermissions = FILE_ALL_ACCESS;
        entry.grfAccessMode = GRANT_ACCESS;
        entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        entries.push_back(entry);
    };
    grant(user_sid);
    for (const auto type : others) {
        DWORD sid_size = SECURITY_MAX_SID_SIZE;
        auto& sid = sids.emplace_back(sid_size);
        if (!::CreateWellKnownSid(type, nullptr, sid.data(), &sid_size)) return false;
    }
    for (auto& sid : sids) grant(sid.data());
    PACL acl = nullptr;
    if (::SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &acl) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR descriptor{};
    bool ok = ::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
              ::SetSecurityDescriptorOwner(&descriptor, user_sid, FALSE) &&
              ::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) &&
              ::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED);
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    ok = ok && ::CreateDirectoryW(path.c_str(), &attributes);
    ::LocalFree(acl);
    return ok;
}
#endif
} // namespace

TEST_CASE("shared_file: a published mapping survives moves and independent opens") {
    test::ScratchDir scratch("shared_mapping");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path blob = cache / "asset.blob";
    auto temporary = platform::CacheTempFile::create(cache);
    REQUIRE(temporary);
    REQUIRE(temporary->write(bytes("asset bytes")));
    REQUIRE(temporary->publish(blob) == platform::PublishResult::Published);
    CHECK(temporary_count(cache) == 0);
    CHECK_FALSE(temporary->write(bytes("late write")));

    platform::ReadOnlyMapping owner;
    const std::byte* original = nullptr;
    {
        auto initial = platform::ReadOnlyMapping::open(blob);
        REQUIRE(initial);
        original = initial->bytes().data();
        owner = std::move(*initial);
        CHECK(initial->bytes().empty());
    }
    CHECK(owner.bytes().data() == original);
    CHECK(mapped_text(owner) == "asset bytes");
    auto other = platform::ReadOnlyMapping::open(blob);
    REQUIRE(other);
    CHECK(mapped_text(*other) == "asset bytes");
    platform::ReadOnlyMapping moved(std::move(owner));
    CHECK(owner.bytes().empty());
    CHECK(moved.bytes().data() == original);
    CHECK(mapped_text(moved) == "asset bytes");
}

TEST_CASE("shared_file: publication never replaces a content key and abandoned files are removed") {
    test::ScratchDir scratch("shared_publish");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path blob = cache / "same-key.blob";
    {
        auto first = platform::CacheTempFile::create(cache);
        REQUIRE(first);
        REQUIRE(first->write(bytes("original")));
        REQUIRE(first->publish(blob) == platform::PublishResult::Published);
    }
    auto existing = platform::ReadOnlyMapping::open(blob);
    REQUIRE(existing);
    {
        auto competitor = platform::CacheTempFile::create(cache);
        REQUIRE(competitor);
        REQUIRE(competitor->write(bytes("replacement")));
        CHECK(competitor->publish(blob) == platform::PublishResult::Exists);
        CHECK(mapped_text(*existing) == "original");
        auto winner = platform::ReadOnlyMapping::open(blob);
        REQUIRE(winner);
        CHECK(mapped_text(*winner) == "original");
    }
    CHECK(temporary_count(cache) == 0);
    {
        auto abandoned = platform::CacheTempFile::create(cache);
        REQUIRE(abandoned);
        REQUIRE(abandoned->write(bytes("partial")));
        CHECK(temporary_count(cache) == 1);
        platform::CacheTempFile moved(std::move(*abandoned));
        CHECK_FALSE(abandoned->write(bytes("moved")));
        CHECK(temporary_count(cache) == 1);
    }
    CHECK(temporary_count(cache) == 0);
    {
        auto wrong_folder = platform::CacheTempFile::create(cache);
        REQUIRE(wrong_folder);
        REQUIRE(wrong_folder->write(bytes("outside")));
        CHECK(wrong_folder->publish(fs::canonical(scratch.path()) / "outside.blob") == platform::PublishResult::Error);
    }
    CHECK(temporary_count(cache) == 0);
    CHECK_FALSE(fs::exists(scratch.path() / "outside.blob"));
}

TEST_CASE("shared_file: shared and exclusive locks are scoped and stable") {
    test::ScratchDir scratch("shared_lock");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path lock = cache / "use.lock";
    {
        auto first = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared);
        auto second = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared);
        REQUIRE(first);
        REQUIRE(second);
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        first.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        platform::ScopedFileLock moved(std::move(*second));
        second.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
    }
    REQUIRE(fs::is_regular_file(lock));
    {
        auto exclusive = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive);
        REQUIRE(exclusive);
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared));
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        platform::ScopedFileLock moved;
        moved = std::move(*exclusive);
        exclusive.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared));
    }
    CHECK(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive).has_value());
    CHECK(fs::is_regular_file(lock));
}

TEST_CASE("shared_file: private directories reject existing public leaves and unexpected types") {
    test::ScratchDir scratch("shared_private");
    const fs::path base = fs::canonical(scratch.path());
    const fs::path nested = base / "new" / "private";
    REQUIRE(platform::prepare_private_cache_directory(nested));
    CHECK(platform::prepare_private_cache_directory(nested));
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(nested, platform::FileLockMode::Shared));
    const fs::path file = nested / "file";
    write_file(file, "x");
    CHECK_FALSE(platform::prepare_private_cache_directory(file));
    CHECK_FALSE(platform::prepare_private_cache_directory(file / "child"));
    CHECK_FALSE(platform::CacheTempFile::create(file));
    write_file(nested / "empty", "");
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested / "empty"));
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested / "missing"));
#if !defined(_WIN32)
    const fs::path public_leaf = base / "public";
    fs::create_directory(public_leaf);
    fs::permissions(public_leaf, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::replace);
    const auto permissions = fs::status(public_leaf).permissions();
    CHECK_FALSE(platform::prepare_private_cache_directory(public_leaf));
    CHECK(fs::status(public_leaf).permissions() == permissions);
    CHECK_FALSE(platform::CacheTempFile::create(public_leaf));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(public_leaf / "lock", platform::FileLockMode::Shared));
    CHECK((fs::status(nested).permissions() & fs::perms::all) == fs::perms::owner_all);
#else
    // A leaf granting this user and LocalSystem is private (Wine reports such an entry on every DACL); one that
    // also grants Everyone is not.
    REQUIRE(create_with_dacl(base / "system", {WinLocalSystemSid}));
    CHECK(platform::prepare_private_cache_directory(base / "system"));
    REQUIRE(create_with_dacl(base / "public", {WinLocalSystemSid, WinWorldSid}));
    CHECK_FALSE(platform::prepare_private_cache_directory(base / "public"));
    CHECK_FALSE(platform::CacheTempFile::create(base / "public"));
#endif
}

TEST_CASE("shared_file: symlinks cannot redirect mappings locks directories or publication") {
    test::ScratchDir scratch("shared_links");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path original = cache / "original";
    const fs::path link = cache / "link";
    write_file(original, "untouched");
    std::error_code ec;
    fs::create_symlink(original, link, ec);
    if (ec) {
        MESSAGE("This environment cannot create a symlink; link-specific checks did not run: " << ec.message());
        return;
    }
    CHECK_FALSE(platform::ReadOnlyMapping::open(link));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(link, platform::FileLockMode::Exclusive));
    auto temporary = platform::CacheTempFile::create(cache);
    REQUIRE(temporary);
    REQUIRE(temporary->write(bytes("replacement")));
    CHECK(temporary->publish(link) == platform::PublishResult::Exists);
    CHECK(fs::is_symlink(link));
    auto untouched = platform::ReadOnlyMapping::open(original);
    REQUIRE(untouched);
    CHECK(mapped_text(*untouched) == "untouched");

    const fs::path alias = fs::canonical(scratch.path()) / "alias";
    fs::create_directory_symlink(cache, alias, ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(platform::prepare_private_cache_directory(alias));
    CHECK_FALSE(platform::prepare_private_cache_directory(alias / "child"));
    CHECK_FALSE(platform::ReadOnlyMapping::open(alias / "original"));
    CHECK_FALSE(platform::CacheTempFile::create(alias));
}

TEST_CASE("shared_file: stamps describe the opened source and detect size time and identity changes") {
    test::ScratchDir scratch("shared_stamp");
    const fs::path original = scratch.path() / "original";
    const fs::path other = scratch.path() / "other";
    write_file(original, "first");
    write_file(other, "first");
    std::FILE* opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto initial = platform::file_stamp(opened);
    const auto repeated = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(initial);
    CHECK(initial == repeated);
    CHECK(initial->size == 5);
    std::FILE* separate = platform::open_c_file(other, "rb");
    REQUIRE(separate);
    const auto different = platform::file_stamp(separate);
    std::fclose(separate);
    REQUIRE(different);
    CHECK(different->size == initial->size);
    CHECK(different != initial);
    const auto time = fs::last_write_time(original);
    fs::last_write_time(original, time + std::chrono::seconds(2));
    opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto retimed = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(retimed);
    CHECK(retimed->size == initial->size);
    CHECK(retimed != initial);
    write_file(original, "longer contents");
    opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto resized = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(resized);
    CHECK(resized->size == 15);
    CHECK(resized != retimed);
    CHECK_FALSE(platform::file_stamp(nullptr));
}

#if !defined(_WIN32)
TEST_CASE("shared_file: ScratchDir isolates permissions under umask 0002") {
    struct ScopedUmask {
        mode_t previous;
        explicit ScopedUmask(mode_t mask) : previous(::umask(mask)) {}
        ~ScopedUmask() { ::umask(previous); }
    };
    ScopedUmask permissive(0002);
    test::ScratchDir scratch("shared_umask_0002");
    const auto perms = fs::status(scratch.path()).permissions() & fs::perms::all;
    CHECK(perms == fs::perms::owner_all);
    const fs::path cache = fs::canonical(scratch.path()) / "nested" / "cache";
    CHECK(platform::prepare_private_cache_directory(cache));
    CHECK((fs::status(cache).permissions() & fs::perms::all) == fs::perms::owner_all);
}

TEST_CASE("shared_file: ancestor policy allows user private-group 0775 but rejects shared and world-writable directories") {
    using platform::detail::PosixGroupRecord;
    using platform::detail::PosixUserRecord;
    using platform::detail::is_safe_ancestor_mode;
    using platform::detail::is_user_private_group_record;

    // Pure policy checks covering UPG vs genuinely shared multi-user groups and permission modes.
    const PosixUserRecord alice{1000, 1000, "alice"};
    const std::array<std::string_view, 0> no_members{};
    const std::array<std::string_view, 1> self_only{"alice"};
    const std::array<std::string_view, 2> shared_members{"alice", "bob"};
    const std::array<std::string_view, 1> other_member{"bob"};

    CHECK(is_user_private_group_record(alice, PosixGroupRecord{1000, "alice", no_members}));
    CHECK(is_user_private_group_record(alice, PosixGroupRecord{1000, "alice", self_only}));
    CHECK_FALSE(is_user_private_group_record(alice, PosixGroupRecord{1000, "alice", shared_members}));
    CHECK_FALSE(is_user_private_group_record(alice, PosixGroupRecord{1000, "alice", other_member}));
    CHECK_FALSE(is_user_private_group_record(alice, PosixGroupRecord{1000, "users", no_members}));
    CHECK_FALSE(is_user_private_group_record(alice, PosixGroupRecord{100, "alice", no_members}));

    // Standard safe modes
    CHECK(is_safe_ancestor_mode(0755, 1000, 1000, 1000, false));
    CHECK(is_safe_ancestor_mode(0755, 0, 0, 1000, false));
    CHECK(is_safe_ancestor_mode(01777, 0, 0, 1000, false));
    // User-owned UPG 0775 is accepted; genuinely shared 0775 is rejected.
    CHECK(is_safe_ancestor_mode(0775, 1000, 1000, 1000, true));
    CHECK_FALSE(is_safe_ancestor_mode(0775, 1000, 100, 1000, false));
    // Root-owned 0775 for non-root user, foreign-owned 0775, and non-sticky 0777 are rejected.
    CHECK_FALSE(is_safe_ancestor_mode(0775, 0, 0, 1000, true));
    CHECK_FALSE(is_safe_ancestor_mode(0775, 1001, 1001, 1000, true));
    CHECK_FALSE(is_safe_ancestor_mode(0777, 1000, 1000, 1000, true));

    // Real filesystem checks inside an isolated ScratchDir.
    test::ScratchDir scratch("shared_upg_fs");
    const fs::path base = fs::canonical(scratch.path());

    // World-writable non-sticky ancestor must always fail.
    const fs::path world_parent = base / "world_writable";
    fs::create_directory(world_parent);
    fs::permissions(world_parent, fs::perms::all, fs::perm_options::replace);
    CHECK_FALSE(platform::prepare_private_cache_directory(world_parent / "cache"));

    // A 0775 directory owned by the current user cannot be accepted as a private leaf (leaf must remain 0700).
    const fs::path upg_parent = base / "upg_parent";
    fs::create_directory(upg_parent);
    fs::permissions(upg_parent,
                    fs::perms::owner_all | fs::perms::group_all | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    CHECK_FALSE(platform::prepare_private_cache_directory(upg_parent));

    // Check whether the current user has a true User Private Group on this host.
    struct stat st{};
    REQUIRE(::lstat(upg_parent.c_str(), &st) == 0);
    const struct passwd* pw = ::getpwuid(::geteuid());
    const struct group* gr = pw ? ::getgrgid(st.st_gid) : nullptr;
    bool host_has_upg = false;
    if (pw && gr && pw->pw_name && gr->gr_name) {
        std::vector<std::string_view> members;
        if (gr->gr_mem) {
            for (char** m = gr->gr_mem; *m; ++m) members.emplace_back(*m);
        }
        host_has_upg = is_user_private_group_record(
            PosixUserRecord{static_cast<std::uint32_t>(pw->pw_uid), static_cast<std::uint32_t>(pw->pw_gid), pw->pw_name},
            PosixGroupRecord{static_cast<std::uint32_t>(gr->gr_gid), gr->gr_name, members});
    }
    if (host_has_upg) {
        CHECK(platform::prepare_private_cache_directory(upg_parent / "cache"));
        // Ancestor permissions must not be modified; leaf must be strictly 0700.
        CHECK((fs::status(upg_parent).permissions() & fs::perms::all) ==
              (fs::perms::owner_all | fs::perms::group_all | fs::perms::others_read | fs::perms::others_exec));
        CHECK((fs::status(upg_parent / "cache").permissions() & fs::perms::all) == fs::perms::owner_all);
    }

    // Real filesystem check for a genuinely shared non-UPG group when a different group can be assigned.
    std::vector<gid_t> candidate_gids;
    int ngroups = ::getgroups(0, nullptr);
    if (ngroups > 0) {
        std::vector<gid_t> groups(static_cast<std::size_t>(ngroups));
        if (::getgroups(ngroups, groups.data()) > 0) {
            for (gid_t g : groups) {
                if (g != st.st_gid) candidate_gids.push_back(g);
            }
        }
    }
    if (::geteuid() == 0) {
        for (const char* name : {"staff", "users", "daemon", "bin", "sys", "adm", "nogroup"}) {
            if (const struct group* alt = ::getgrnam(name); alt && alt->gr_gid != st.st_gid)
                candidate_gids.push_back(alt->gr_gid);
        }
    }
    for (gid_t shared_gid : candidate_gids) {
        const fs::path shared_parent = base / ("shared_group_" + std::to_string(shared_gid));
        fs::create_directory(shared_parent);
        if (::chown(shared_parent.c_str(), static_cast<uid_t>(-1), shared_gid) == 0) {
            fs::permissions(shared_parent,
                            fs::perms::owner_all | fs::perms::group_all | fs::perms::others_read |
                                fs::perms::others_exec,
                            fs::perm_options::replace);
            CHECK_FALSE(platform::prepare_private_cache_directory(shared_parent / "cache"));
            break;
        }
    }
}
#endif
