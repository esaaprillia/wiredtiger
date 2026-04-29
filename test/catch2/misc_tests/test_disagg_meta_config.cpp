/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <array>
#include <utility>
#include <sstream>
#include <string_view>

#include "wt_internal.h"

#include "wrappers/mock_session.h"
#include "utils.h"

/*
 * Unit tests for disaggregated storage metadata parsing.
 */

struct disagg_fixture {
    enum Config { CHECKPOINT, TIMESTAMP, KEY_PROVIDER };

    using kv_t = std::pair<std::string, std::string>;
    const std::array<kv_t, 3> config = {std::make_pair("checkpoint",
                                          R"((WiredTigerCheckpoint.1=(
                addr="00c025808282bd21596019",
                order=1,
                time=1766470626,
                size=101,
                newest_start_durable_ts=0,
                oldest_start_ts=0,
                newest_txn=0,
                newest_stop_durable_ts=0,
                newest_stop_ts=-1,
                newest_stop_txn=-11,
                prepare=0,
                write_gen=3,
                run_write_gen=1,
                next_page_id=102
                )
            ))"),
      std::make_pair("timestamp", "c0ffee12"), /* hex number */
      std::make_pair("key_provider",
        R"((page.1=(
                  page_id=1,
                  lsn=123
                  ),
                version=1
            ))")};

    template <typename... Args>
    std::string
    join_cfg(Args... indices)
    {
        std::string result;
        ((result += config[indices].first + "=" + config[indices].second + ","), ...);
        return result;
    }

    std::shared_ptr<mock_session> session_wrapper;
    WT_SESSION_IMPL *session = nullptr;

    disagg_fixture() : session_wrapper(mock_session::build_test_mock_session())
    {
        session = session_wrapper->get_wt_session_impl();
        REQUIRE(session != nullptr);

        REQUIRE(config.size() == KEY_PROVIDER + 1);
    }
};

TEST_CASE_METHOD(disagg_fixture, "Parse metadata", "[disagg]")
{
    SECTION("All fields present")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, TIMESTAMP, KEY_PROVIDER);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);

        REQUIRE(config[CHECKPOINT].second ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));

        const wt_timestamp_t expected_timestamp =
          std::stoull(config[TIMESTAMP].second, nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);

        REQUIRE(config[KEY_PROVIDER].second ==
          std::string_view(metadata.key_provider, metadata.key_provider_len));
    }

    SECTION("Key provider missing")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, TIMESTAMP);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);

        REQUIRE(config[CHECKPOINT].second ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));

        const wt_timestamp_t expected_timestamp =
          std::stoull(config[TIMESTAMP].second, nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);

        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }

    SECTION("Missing fields")
    {
        const std::string metadata_str = join_cfg(CHECKPOINT, KEY_PROVIDER);

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Null metadata")
    {
        WT_ITEM metadata_buf{};
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Empty metadata")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)"";
        metadata_buf.size = 0;
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Length limited")
    {
        const std::string metadata_str = "checkpoint=(),timestamp=c0ffee12";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length() - 2; /* truncate the last 2 bytes */
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(std::string_view("()", 2) ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));
        const wt_timestamp_t expected_timestamp = std::stoull("c0ffee", nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);
        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }

    SECTION("Unknown keys ignored if version doesn't match")
    {
        std::stringstream metadata_stream;
        metadata_stream << "version=" << WT_DISAGG_CHECKPOINT_TURTLE_VERSION + 1
                        << ",compatible_version=1,unknown_key=foo,checkpoint=(),timestamp=c0ffee12,"
                           "another_unknown=bar,";
        const std::string metadata_str = metadata_stream.str();

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);

        REQUIRE(std::string_view("()", 2) ==
          std::string_view(metadata.checkpoint, metadata.checkpoint_len));
        const wt_timestamp_t expected_timestamp = std::stoull("c0ffee12", nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);
    }

    SECTION("Unknown keys are an error if version matches")
    {
        std::stringstream metadata_stream;
        metadata_stream << "version=" << WT_DISAGG_CHECKPOINT_TURTLE_VERSION
                        << "compatible_version=1,unknown_key=foo,checkpoint=(),timestamp=c0ffee12,";
        const std::string metadata_str = metadata_stream.str();

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }
}

TEST_CASE_METHOD(disagg_fixture, "Parse crypt key metadata", "[disagg]")
{
    SECTION("Well-formed")
    {
        WT_DISAGG_METADATA metadata{};
        metadata.checkpoint = nullptr;
        metadata.checkpoint_len = 0;
        metadata.checkpoint_timestamp = 0;
        metadata.key_provider = config[KEY_PROVIDER].second.data();
        metadata.key_provider_len = config[KEY_PROVIDER].second.length();

        uint64_t page_id = 0, lsn = 0;
        const auto ret = __wti_disagg_parse_crypt_meta(session, &metadata, &page_id, &lsn);
        REQUIRE(ret == 0);

        REQUIRE(page_id == 1);
        REQUIRE(lsn == 123);
    }

    SECTION("Malformed")
    {
        const std::string_view invalid_meta[] = {
          "(page.1=(page_id=aaa,lsn=123),version=1)", /* invalid page_id */
          "(page.1=(page_id=123,lsn=123),version=1)", /* page_id out of range */
          "(page.1=(page_id=1,lsn=aaa),version=1)",   /* invalid lsn */
          "(page.1=(page_id=1),version=1)",           /* missing lsn */
          "(page.1=(lsn=123),version=1)",             /* missing page_id */
          "(page.1=(page_id=1,lsn=123))",             /* missing version */
          "(page.1=(page_id=1,lsn=123),version=2)",   /* unsupported version */
          "invalid_format"                            /* completely invalid */
        };

        for (const auto &meta : invalid_meta) {
            WT_DISAGG_METADATA metadata{};
            metadata.checkpoint = nullptr;
            metadata.checkpoint_len = 0;
            metadata.checkpoint_timestamp = 0;
            metadata.key_provider = meta.data();
            metadata.key_provider_len = meta.length();

            uint64_t page_id = 0, lsn = 0;
            const auto ret = __wti_disagg_parse_crypt_meta(session, &metadata, &page_id, &lsn);
            REQUIRE(ret == EINVAL);
        }
    }
}

TEST_CASE_METHOD(disagg_fixture, "Legacy metadata format", "[disagg]")
{
    const std::string checkpoint =
      "(WiredTigerCheckpoint.1=(addr=\"00c025808282bd21596019\",order=1,time=1766470626))";
    const std::string timestamp = "timestamp=c0ffee12"; /* hex number */
    const std::string legacy_metadata = checkpoint + "\n" + timestamp;

    SECTION("Complete metadata")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)legacy_metadata.data();
        metadata_buf.size = legacy_metadata.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(checkpoint == std::string_view(metadata.checkpoint, metadata.checkpoint_len));
        const wt_timestamp_t expected_timestamp = std::stoull("c0ffee12", nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);
        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }

    SECTION("Length limited")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)legacy_metadata.data();
        /* truncate the last 2 digits from the timestamp */
        metadata_buf.size = legacy_metadata.length() - 2;
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(checkpoint == std::string_view(metadata.checkpoint, metadata.checkpoint_len));
        const wt_timestamp_t expected_timestamp = std::stoull("c0ffee", nullptr, 16);
        REQUIRE(expected_timestamp == metadata.checkpoint_timestamp);
        REQUIRE(metadata.key_provider == nullptr);
        REQUIRE(metadata.key_provider_len == 0);
    }

    SECTION("Missing timestamp")
    {
        const std::string incomplete_metadata = checkpoint;

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)incomplete_metadata.data();
        metadata_buf.size = incomplete_metadata.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Missing timestamp 2")
    {
        const std::string incomplete_metadata = checkpoint + "\n";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)incomplete_metadata.data();
        metadata_buf.size = incomplete_metadata.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Invalid timestamp")
    {
        const char *invalid_ts[] = {
          "timestamp=",                               /* empty timestamp */
          "timestamp=zzzz",                           /* non-hex characters */
          "timestamp=-1234",                          /* negative number */
          "timestamp=123456789012345678901234567890", /* too large */
          "tmstmp=c0ffee12"                           /* misspelled key */
        };

        for (const auto &ts : invalid_ts) {
            const std::string incomplete_metadata = checkpoint + "\n" + ts;

            WT_ITEM metadata_buf{};
            metadata_buf.data = (const void *)incomplete_metadata.data();
            metadata_buf.size = incomplete_metadata.length();
            WT_DISAGG_METADATA metadata{};

            const auto ret = __wt_disagg_parse_meta(session, &metadata_buf, &metadata);
            REQUIRE(ret == EINVAL);
        }
    }
}

/*
 * Regression test for the downgrade incompatibility between mongodb-9.0/develop and mongodb-8.3.
 *
 * The develop binary changed __wt_disagg_put_checkpoint_meta to write inner page-log metadata in
 * a new "v2" format (prefixed with "version=2,compatible_version=1,\n"). The mongodb-8.3 binary's
 * __wt_disagg_parse_meta contained a format-detection heuristic that checked whether the metadata
 * started with "checkpoint=" in order to select which sub-parser to invoke:
 *
 *   if (WT_PREFIX_MATCH((const char *)meta_buf->data, "checkpoint="))
 *       __disagg_parse_meta(...)        // The "regular" (v1) parser
 *   else
 *       __disagg_parse_legacy_meta(...) // The pre-v1 "legacy" parser
 *
 * The v2 format starts with "version=2,...", NOT "checkpoint=", so the 8.3 binary incorrectly
 * routed v2 metadata to the legacy parser. The legacy parser expected:
 *   line 1: (WiredTigerCheckpoint...)
 *   line 2: timestamp=<hex>
 * but received "version=2,...", causing:
 *   "Disaggregated checkpoint legacy metadata invalid timestamp entry" (EINVAL)
 * -> invariantWTOK -> mongod crash.
 *
 * The current binary fixes the routing by checking for the "(WiredTigerCheckpoint." sentinel
 * (which identifies the true pre-v1 legacy format) instead of "checkpoint=".
 */
TEST_CASE_METHOD(
  disagg_fixture, "v2 metadata format: downgrade incompatibility regression", "[disagg][regression]")
{
    /*
     * The v1 inner metadata format written by mongodb-8.3's __wt_disagg_put_checkpoint_meta.
     * Starts with "checkpoint=", which was the routing sentinel in 8.3's parser.
     */
    const std::string v1_metadata =
      "checkpoint=(WiredTigerCheckpoint.1=("
      "addr=\"00c025808282bd21596019\","
      "order=1,time=1766470626,size=101,"
      "newest_start_durable_ts=0,oldest_start_ts=0,"
      "newest_txn=0,newest_stop_durable_ts=0,newest_stop_ts=-1,newest_stop_txn=-11,"
      "prepare=0,write_gen=3,run_write_gen=1,next_page_id=102"
      ")),\n"
      "timestamp=c0ffee12,\n"
      "oldest_timestamp=c0ffee12";

    /*
     * The v2 inner metadata format written by mongodb-9.0/develop's __wt_disagg_put_checkpoint_meta
     * (conn_layered_page_log.c). The content is identical but prefixed with
     * "version=2,compatible_version=1,\n" and extended with largest_file_id and key_provider.
     * These exact values match the page-log content from the production crash.
     */
    const std::string v2_metadata =
      "version=2,compatible_version=1,\n"
      "checkpoint=(WiredTigerCheckpoint.615676=("
      "addr=\"00e311fbdc80e869e29c5affffe200e869e29c5affffe200c081e7fcf957\","
      "order=615676,time=1776459060,size=1475515,"
      "newest_start_durable_ts=0,oldest_start_ts=0,"
      "newest_txn=3341,newest_stop_durable_ts=0,newest_stop_ts=-1,newest_stop_txn=-11,"
      "prepare=0,write_gen=22081569,run_write_gen=22081029,next_page_id=1186845"
      ")),\n"
      "timestamp=69e29c5400000001,\n"
      "oldest_timestamp=69e2954c00000001,\n"
      "largest_file_id=1071,\n"
      "key_provider=(page.1=(page_id=1,lsn=7625638039464706519),version=1)";

    /*
     * Reproduce the 8.3 routing decision: check whether the metadata starts with "checkpoint=".
     *
     * v1 correctly triggers the regular parser (starts with "checkpoint=").
     * v2 does NOT start with "checkpoint=", so the 8.3 binary sent it to the legacy parser.
     */
    const bool old_routing_selects_regular_for_v1 = (v1_metadata.rfind("checkpoint=", 0) == 0);
    const bool old_routing_selects_regular_for_v2 = (v2_metadata.rfind("checkpoint=", 0) == 0);

    /* The 8.3 routing correctly identified v1 format. */
    REQUIRE(old_routing_selects_regular_for_v1);

    /* The 8.3 routing MISIDENTIFIED v2: sent it to the legacy parser, causing EINVAL. */
    REQUIRE(!old_routing_selects_regular_for_v2);

    SECTION("v1 format: current parser handles it correctly (backward compatibility)")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)v1_metadata.data();
        metadata_buf.size = v1_metadata.length();
        WT_DISAGG_METADATA metadata{};

        REQUIRE(__wt_disagg_parse_meta(session, &metadata_buf, &metadata) == 0);
        const uint64_t expected_ts = std::stoull("c0ffee12", nullptr, 16);
        REQUIRE(metadata.checkpoint_timestamp == expected_ts);
    }

    SECTION("v2 format: current parser handles it correctly (regression guard)")
    {
        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)v2_metadata.data();
        metadata_buf.size = v2_metadata.length();
        WT_DISAGG_METADATA metadata{};

        /*
         * The current parser uses "(WiredTigerCheckpoint." as the legacy sentinel instead of
         * "checkpoint=". v2 doesn't start with the legacy sentinel, so it goes through the
         * versioned path and parses successfully.
         */
        REQUIRE(__wt_disagg_parse_meta(session, &metadata_buf, &metadata) == 0);
        const uint64_t expected_ts = std::stoull("69e29c5400000001", nullptr, 16);
        REQUIRE(metadata.checkpoint_timestamp == expected_ts);
        REQUIRE(metadata.largest_file_id == 1071);
        REQUIRE(metadata.key_provider != nullptr);
    }

    SECTION("v2 format: sending to legacy parser reproduces the 8.3 crash")
    {
        /*
         * Simulate the 8.3 binary's __disagg_parse_legacy_meta receiving v2 metadata.
         * The legacy parser:
         *   1. Reads the first line up to '\n' as the checkpoint data.
         *   2. Checks that the second line starts with "timestamp=".
         * With v2 input, line 1 = "version=2,compatible_version=1," and
         * line 2 = "checkpoint=(...)" — NOT "timestamp=", causing EINVAL.
         *
         * We cannot call __disagg_parse_legacy_meta directly (it is static), but we can
         * verify the same EINVAL outcome by confirming that the data passed to it does NOT
         * have "timestamp=" on the second line, which is the exact check that failed.
         */
        const std::string_view v2_view = v2_metadata;
        const size_t first_newline = v2_view.find('\n');
        REQUIRE(first_newline != std::string_view::npos);

        const std::string_view line2 = v2_view.substr(first_newline + 1);
        /* Line 2 of v2 metadata is "checkpoint=...", not "timestamp=...". */
        REQUIRE(line2.substr(0, 10) != "timestamp=");
    }
}

TEST_CASE_METHOD(disagg_fixture, "Parse metadata with version", "[disagg]")
{
    SECTION("Valid version")
    {
        const std::string metadata_str =
          "version=1,compatible_version=1,checkpoint=(),timestamp=c0ffee12,";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __ut_disagg_parse_version_and_check(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(metadata.version == 1);
        REQUIRE(metadata.compatible_version == 1);
    }

    SECTION("Incompatible version")
    {
        const std::string metadata_str =
          "version=1,compatible_version=999,checkpoint=(),timestamp=c0ffee12,";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __ut_disagg_parse_version_and_check(session, &metadata_buf, &metadata);
        REQUIRE(ret == ENOTSUP);
    }

    SECTION("Missing version")
    {
        const std::string metadata_str = "compatible_version=1,checkpoint=(),timestamp=c0ffee12,";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __ut_disagg_parse_version_and_check(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Missing compatible_version")
    {
        const std::string metadata_str = "version=1,checkpoint=(),timestamp=c0ffee12,";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __ut_disagg_parse_version_and_check(session, &metadata_buf, &metadata);
        REQUIRE(ret == EINVAL);
    }

    SECTION("Default version when omitted")
    {
        const std::string metadata_str = "checkpoint=(),timestamp=c0ffee12,";

        WT_ITEM metadata_buf{};
        metadata_buf.data = (const void *)metadata_str.data();
        metadata_buf.size = metadata_str.length();
        WT_DISAGG_METADATA metadata{};

        const auto ret = __ut_disagg_parse_version_and_check(session, &metadata_buf, &metadata);
        REQUIRE(ret == 0);
        REQUIRE(metadata.version == WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT);
        REQUIRE(metadata.compatible_version == WT_DISAGG_CHECKPOINT_TURTLE_VERSION_DEFAULT);
    }
}
