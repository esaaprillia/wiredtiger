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
    explicit home_directory(const std::string_view path) : _path(path)
    {
        std::filesystem::remove_all(path);
    }

    [[nodiscard]] std::string_view
    path() const
    {
        return _path;
    }

private:
    std::string _path;
};

class write_conflict_fixture {
public:
    write_conflict_fixture()
    {
        constexpr auto uri = "layered:write_conflict";

        static constexpr auto config =
          "key_format=S,value_format=S,block_manager=disagg,type=layered";

        auto &session = _session->iface;
        REQUIRE(session.create(&session, uri, config) == 0);
        REQUIRE(session.open_cursor(&session, uri, nullptr, nullptr, &_cursor) == 0);
    }

    [[nodiscard]] WT_SESSION_IMPL *
    session() const
    {
        return _session;
    }

    [[nodiscard]] WT_SESSION_IMPL *
    create_session()
    {
        return _conn.create_session();
    }

    [[nodiscard]] WT_LAYERED_TABLE *
    layered_table() const
    {
        auto *layered_cursor = reinterpret_cast<WT_CURSOR_LAYERED *>(_cursor);
        return reinterpret_cast<WT_LAYERED_TABLE *>(layered_cursor->dhandle);
    }

private:
    static constexpr auto conn_config =
      "create,"
      "extensions=[./ext/page_log/palite/libwiredtiger_palite.so],"
      "disaggregated=(role=follower,page_log=palite)";

    scoped_fast_truncate_enable _enable;
    home_directory _home{"WT_TEST.truncate_write_conflict"};
    connection_wrapper _conn{_home.path().data(), conn_config};
    WT_SESSION_IMPL *_session{_conn.create_session()};
    WT_CURSOR *_cursor{};
};

template <typename Op>
int
do_in_transaction(WT_SESSION_IMPL *s, const Op operation, const bool commit)
{
    auto *iface = &s->iface;
    REQUIRE(iface->begin_transaction(iface, nullptr) == 0);

    const int ret = operation();

    if (commit)
        REQUIRE(iface->commit_transaction(iface, nullptr) == 0);

    return ret;
}

template <typename Op>
int
do_in_uncommitted_transaction(WT_SESSION_IMPL *session, const Op operation)
{
    return do_in_transaction(session, operation, /* commit = */ false);
}

template <typename Op>
int
do_in_committed_transaction(WT_SESSION_IMPL *session, const Op operation)
{
    return do_in_transaction(session, operation, /* commit = */ true);
}

void
insert_entry(WT_SESSION_IMPL *s, WT_LAYERED_TABLE *t, std::string_view start, std::string_view stop)
{
    auto start_item = make_item(start);
    auto stop_item = make_item(stop);
    REQUIRE(__wt_insert_truncate_entry(s, t, &start_item, &stop_item) == 0);
}

int
detect_conflict(WT_SESSION_IMPL *s, WT_LAYERED_TABLE *t, std::string_view key)
{
    auto key_item = make_item(key);
    return __wt_layered_table_truncate_detect_write_conflict(s, t, &key_item);
}

} // namespace

SCENARIO("write conflict returns 0 for an empty truncate list", "[truncate_list][write_conflict]")
{
    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;

        WHEN("the conflict check is called for any key")
        {
            const auto result = do_in_uncommitted_transaction(f.session(), [&] {
                return detect_conflict(f.session(), f.layered_table(), "key150");
            });

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
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            return 0;
        });

        WHEN("the conflict check is called for a key before the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key050");
            });

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key after the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key250");
            });

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }

    GIVEN("two non-overlapping uncommitted ranges [key100, key200] and [key400, key500]")
    {
        write_conflict_fixture f;
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            insert_entry(f.session(), f.layered_table(), "key400", "key500");
            return 0;
        });

        WHEN("the conflict check is called for a key between the ranges")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key300");
            });

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
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            return 0;
        });

        WHEN("the conflict check is called for a key strictly inside the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key150");
            });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the start boundary key")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key100");
            });

            THEN("it returns WT_ROLLBACK (start boundary is inclusive)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for the stop boundary key")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key200");
            });

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
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key100");
            return 0;
        });

        WHEN("the conflict check is called for the exact key")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key100");
            });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key just before the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key099");
            });

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key just after the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key101");
            });

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }
    }
}

SCENARIO(
  "write conflict with two non-overlapping uncommitted ranges", "[truncate_list][write_conflict]")
{
    GIVEN("uncommitted ranges [key100, key200] and [key400, key500]")
    {
        write_conflict_fixture f;
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            insert_entry(f.session(), f.layered_table(), "key400", "key500");
            return 0;
        });

        WHEN("the conflict check is called for a key in the first range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key150");
            });

            THEN("it returns WT_ROLLBACK")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key in the second range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key450");
            });

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
        do_in_committed_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            return 0;
        });

        WHEN("the conflict check is called for a key inside the committed range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key150");
            });

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

        WHEN("the conflict check is called for a key inside that range")
        {
            const auto result = do_in_uncommitted_transaction(f.session(), [&] {
                insert_entry(f.session(), f.layered_table(), "key100", "key200");
                return detect_conflict(f.session(), f.layered_table(), "key150");
            });

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
        auto *session_2 = f.create_session();
        do_in_committed_transaction(session_2, [&] {
            insert_entry(session_2, f.layered_table(), "key100", "key300");
            return 0;
        });
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key200", "key400");
            return 0;
        });

        WHEN("the conflict check is called for a key covered only by the committed range")
        {
            auto *session_3 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_3, [&] {
                return detect_conflict(session_3, f.layered_table(), "key150");
            });

            THEN("it returns 0")
            {
                REQUIRE(result == 0);
            }
        }

        WHEN("the conflict check is called for a key in the overlap region")
        {
            auto *session_3 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_3, [&] {
                return detect_conflict(session_3, f.layered_table(), "key250");
            });

            THEN("it returns WT_ROLLBACK (uncommitted range covers the key)")
            {
                REQUIRE(result == WT_ROLLBACK);
            }
        }

        WHEN("the conflict check is called for a key covered only by the uncommitted range")
        {
            auto *session_3 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_3, [&] {
                return detect_conflict(session_3, f.layered_table(), "key350");
            });

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
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            return 0;
        });

        WHEN("the conflict check detects a write conflict")
        {
            auto *session_2 = f.create_session();
            bool released;
            do_in_uncommitted_transaction(session_2, [&] {
                CHECK(detect_conflict(session_2, f.layered_table(), "key150") == WT_ROLLBACK);
                released = lock_is_released(*session_2, *f.layered_table());
                return 0;
            });

            THEN("the truncate lock is not held")
            {
                REQUIRE(released);
            }
        }

        WHEN("the conflict check finds no conflict")
        {
            auto *session_2 = f.create_session();
            bool released;
            do_in_uncommitted_transaction(session_2, [&] {
                CHECK(detect_conflict(session_2, f.layered_table(), "key050") == 0);
                released = lock_is_released(*session_2, *f.layered_table());
                return 0;
            });

            THEN("the truncate lock is not held")
            {
                REQUIRE(released);
            }
        }
    }

    GIVEN("a layered table with an empty truncate list")
    {
        write_conflict_fixture f;

        WHEN("the conflict check is called")
        {
            bool released;
            do_in_uncommitted_transaction(f.session(), [&] {
                CHECK(detect_conflict(f.session(), f.layered_table(), "key150") == 0);
                released = lock_is_released(*f.session(), *f.layered_table());
                return 0;
            });

            THEN("the truncate lock is not held")
            {
                REQUIRE(released);
            }
        }
    }
}

SCENARIO("write conflict feature flag disabled returns 0", "[truncate_list][write_conflict]")
{
    GIVEN("an uncommitted truncate range exists but the feature flag is disabled")
    {
        write_conflict_fixture f;
        do_in_uncommitted_transaction(f.session(), [&] {
            insert_entry(f.session(), f.layered_table(), "key100", "key200");
            return 0;
        });
        /* Declared after f so it destructs first, restoring the flag before f's connection closes.
         */
        scoped_fast_truncate_enable flag_restore;
        __wt_process.disagg_fast_truncate_2026 = false;

        WHEN("the conflict check is called for a key inside the range")
        {
            auto *session_2 = f.create_session();
            const auto result = do_in_uncommitted_transaction(session_2, [&] {
                return detect_conflict(session_2, f.layered_table(), "key150");
            });

            THEN("it returns 0 (feature flag early exit)")
            {
                REQUIRE(result == 0);
            }
        }
    }
}
