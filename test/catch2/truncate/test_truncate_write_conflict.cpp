/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// Standard include:
#include <filesystem>
#include <string_view>

// External include:
#include <catch2/catch.hpp>

// WiredTiger include:
#include "wt_internal.h"
#include "wrappers/connection_wrapper.h"
#include "truncate_list_helpers.hpp"

using namespace truncate_list_helpers;

namespace {

// Ensures the given test directory is removed at the beginning of the test.
class home_directory {
public:
    explicit home_directory(std::string_view path) : _path(path)
    {
        std::filesystem::remove_all(path);
    }

    [[nodiscard]] const char *
    path() const
    {
        return _path.c_str();
    }

private:
    std::string _path;
};

class write_conflict_fixture {
public:
    write_conflict_fixture()
        : _conn(_home.path(), connection_config), _writer{_conn.create_session()},
          _reader{_conn.create_session()}
    {
        REQUIRE(_writer->iface.create(&_writer->iface, uri, table_config) == 0);
        REQUIRE(_writer->iface.open_cursor(&_writer->iface, uri, nullptr, nullptr, &_cursor) == 0);
    }

    void
    begin_writer()
    {
        REQUIRE(_writer->iface.begin_transaction(&_writer->iface, nullptr) == 0);
    }

    void
    begin_reader()
    {
        REQUIRE(_reader->iface.begin_transaction(&_reader->iface, nullptr) == 0);
    }

    /* Commits immediately; must be called before begin_reader(). */
    void
    add_committed_entry(std::string_view start, std::string_view stop)
    {
        auto *s = _conn.create_session();
        REQUIRE(s->iface.begin_transaction(&s->iface, nullptr) == 0);
        insert(s, start, stop);
        REQUIRE(s->iface.commit_transaction(&s->iface, nullptr) == 0);
    }

    /* Writer's transaction must be open; reader's snapshot will exclude it. */
    void
    add_uncommitted_entry(std::string_view start, std::string_view stop)
    {
        insert(_writer, start, stop);
    }

    /* Reader's transaction must be open; self-visible, no conflict. */
    void
    add_own_uncommitted_entry(std::string_view start, std::string_view stop)
    {
        insert(_reader, start, stop);
    }

    int
    detect_conflict(std::string_view key)
    {
        auto key_item = make_item(key);
        return __wt_layered_table_truncate_detect_write_conflict(
          _reader, &layered_table(), &key_item);
    }

    [[nodiscard]] bool
    lock_is_released()
    {
        return truncate_list_helpers::lock_is_released(*_reader, layered_table());
    }

private:
    void
    insert(WT_SESSION_IMPL *session, std::string_view start, std::string_view stop)
    {
        auto start_item = make_item(start);
        auto stop_item = make_item(stop);
        REQUIRE(
          __wt_insert_truncate_entry(session, &layered_table(), &start_item, &stop_item) == 0);
    }

    [[nodiscard]] WT_LAYERED_TABLE &
    layered_table() const
    {
        return *reinterpret_cast<WT_LAYERED_TABLE *>(
          reinterpret_cast<WT_CURSOR_LAYERED *>(_cursor)->dhandle);
    }

    static constexpr auto uri = "layered:write_conflict";

    static constexpr auto connection_config =
      "create,"
      "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=follower,page_log=palite)";

    static constexpr auto table_config =
      "key_format=S,value_format=S,block_manager=disagg,type=layered";

    home_directory _home{"WT_TEST.truncate_write_conflict"};
    scoped_fast_truncate_enable _enable;
    connection_wrapper _conn;
    WT_SESSION_IMPL *_writer;
    WT_SESSION_IMPL *_reader;
    WT_CURSOR *_cursor{};
};

} // namespace

SCENARIO("write conflict returns 0 for an empty truncate list", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;
        f.begin_reader();

        WHEN("the conflict check is called for any key")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict returns 0 when the key is outside all uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [key100, key200]")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.begin_reader();

        WHEN("the conflict check is called for a key before the range")
        {
            const auto result = f.detect_conflict("key050");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key after the range")
        {
            const auto result = f.detect_conflict("key250");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }

    GIVEN("two non-overlapping uncommitted ranges [key100, key200] and [key400, key500]")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.add_uncommitted_entry("key400", "key500");
        f.begin_reader();

        WHEN("the conflict check is called for a key between the ranges")
        {
            const auto result = f.detect_conflict("key300");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict returns WT_ROLLBACK when the key is inside an uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("one uncommitted truncate range [key100, key200]")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.begin_reader();

        WHEN("the conflict check is called for a key strictly inside the range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the start boundary key")
        {
            const auto result = f.detect_conflict("key100");

            THEN("it returns WT_ROLLBACK (start boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the stop boundary key")
        {
            const auto result = f.detect_conflict("key200");

            THEN("it returns WT_ROLLBACK (stop boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict with a single-key uncommitted range", "[truncate_list][write_conflict]")
{
    GIVEN("a single-key uncommitted range [key100, key100]")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key100");
        f.begin_reader();

        WHEN("the conflict check is called for the exact key")
        {
            const auto result = f.detect_conflict("key100");

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key just before the range")
        {
            const auto result = f.detect_conflict("key099");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key just after the range")
        {
            const auto result = f.detect_conflict("key101");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict with two non-overlapping uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("uncommitted ranges [key100, key200] and [key400, key500]")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.add_uncommitted_entry("key400", "key500");
        f.begin_reader();

        WHEN("the conflict check is called for a key in the first range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key in the second range")
        {
            const auto result = f.detect_conflict("key450");

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict does not trigger for a committed truncate range",
  "[truncate_list][write_conflict]")
{
    GIVEN("one committed (globally visible) truncate range [key100, key200]")
    {
        write_conflict_fixture f;
        f.add_committed_entry("key100", "key200");
        f.begin_reader();

        WHEN("the conflict check is called for a key inside the committed range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict does not trigger for the reader's own uncommitted range",
  "[truncate_list][write_conflict]")
{
    GIVEN("an uncommitted truncate range owned by the current transaction")
    {
        write_conflict_fixture f;
        f.begin_reader();
        f.add_own_uncommitted_entry("key100", "key200");

        WHEN("the conflict check is called for a key inside that range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns 0 (own uncommitted range is self-visible)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO("write conflict with overlapping committed and uncommitted ranges",
  "[truncate_list][write_conflict]")
{
    GIVEN("a committed range [key100, key300] and an uncommitted range [key200, key400]")
    {
        write_conflict_fixture f;
        f.add_committed_entry("key100", "key300");
        f.begin_writer();
        f.add_uncommitted_entry("key200", "key400");
        f.begin_reader();

        WHEN("the conflict check is called for a key covered only by the committed range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key in the overlap region")
        {
            const auto result = f.detect_conflict("key250");

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key covered only by the uncommitted range")
        {
            const auto result = f.detect_conflict("key350");

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }
    }
}

SCENARIO("write conflict read lock is always released", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table with one uncommitted truncate range")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.begin_reader();

        WHEN("the conflict check detects a write conflict")
        {
            CHECK(f.detect_conflict("key150") == WT_ROLLBACK);

            THEN("the truncate lock is not held")
            {
                REQUIRE(f.lock_is_released());
            }
        }

        WHEN("the conflict check finds no conflict")
        {
            CHECK(f.detect_conflict("key050") == 0);

            THEN("the truncate lock is not held")
            {
                REQUIRE(f.lock_is_released());
            }
        }
    }

    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;
        f.begin_reader();

        WHEN("the conflict check is called")
        {
            CHECK(f.detect_conflict("key150") == 0);

            THEN("the truncate lock is not held")
            {
                REQUIRE(f.lock_is_released());
            }
        }
    }
}

SCENARIO("write conflict feature flag disabled returns 0", "[truncate_list][write_conflict]")
{
    GIVEN("an uncommitted truncate range exists but the feature flag is disabled")
    {
        write_conflict_fixture f;
        f.begin_writer();
        f.add_uncommitted_entry("key100", "key200");
        f.begin_reader();
        /* Declared after f so it destructs first, restoring the flag before f's connection closes. */
        scoped_fast_truncate_enable flag_restore;
        __wt_process.disagg_fast_truncate_2026 = false;

        WHEN("the conflict check is called for a key inside the range")
        {
            const auto result = f.detect_conflict("key150");

            THEN("it returns 0 (feature flag early exit)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}
