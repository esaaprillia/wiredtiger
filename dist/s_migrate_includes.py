#!/usr/bin/env python3
"""
Migrate .c files from #include "wt_internal.h" to specific includes.

For each .c file, determines the minimal set of headers needed based on
symbol usage, then rewrites the include to use specific headers.

Files that call inline functions are skipped since those require the full
inline header chain (effectively all of wt_internal.h).
"""

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOP_DIR = os.path.dirname(SCRIPT_DIR)
SRC_DIR = os.path.join(TOP_DIR, "src")
INCLUDE_DIR = os.path.join(SRC_DIR, "include")

EXTERN_FN_PATTERN = re.compile(r'\b__wt[i]?_[a-z_][a-z0-9_]*\b')

# Inline headers that have been made self-contained (use extern_noninline.h
# instead of extern.h) and can be included by migrated files.
LEAF_INLINE_HEADERS = {
    "buf_inline.h",
    "time_inline.h",
    "misc_inline.h",
    "os_fhandle_inline.h",
    "os_fs_inline.h",
    "bitstring_inline.h",
    "ref_inline.h",
    "generation_inline.h",
    "intpack_inline.h",
    "int4bitpack_inline.h",
    "ctype_inline.h",
    "str_inline.h",
    "timestamp_inline.h",
    "modify_inline.h",
    "column_inline.h",
    "block_inline.h",
    "mutex_inline.h",
    "cache_inline.h",
    "packing_inline.h",
    "conf_inline.h",
    "os_fstream_inline.h",
}

def _load_inline_function_names():
    """Load all static inline function names from extern.h."""
    extern_path = os.path.join(INCLUDE_DIR, "extern.h")
    names = set()
    if os.path.isfile(extern_path):
        with open(extern_path) as f:
            for line in f:
                m = re.match(r'^static\s+WT_INLINE\s+\S+\s+\*?(__wt\w+)\s*\(', line)
                if m:
                    names.add(m.group(1))
    return names

INLINE_FUNCTION_NAMES = _load_inline_function_names()

def _load_leaf_inline_function_names():
    """Load function names defined in leaf inline headers."""
    names = {}
    for hdr in LEAF_INLINE_HEADERS:
        path = os.path.join(INCLUDE_DIR, hdr)
        if not os.path.isfile(path):
            continue
        with open(path) as f:
            for line in f:
                m = re.match(r'^(__wt\w+|__col\w+|__bit\w+|__ref\w+|__time\w+)\s*\(', line)
                if m:
                    names[m.group(1)] = hdr
    return names

LEAF_INLINE_FUNCTIONS = _load_leaf_inline_function_names()

# Symbols that indicate the file accesses session/connection struct members
SESSION_PATTERNS = [
    r'\bWT_SESSION_IMPL\b',
    r'\bS2BT\b',
]
CONNECTION_PATTERNS = [
    r'\bWT_CONNECTION_IMPL\b',
    r'\bS2C\b',
]

# Map symbols to the header providing them
SYMBOL_HEADERS = [
    (r'\bWT_GCC_FUNC_ATTRIBUTE\b|\bWT_COMPILER_TYPE_ALIGN\b', "wt_compiler.h"),
    (r'\bWT_FULL_BARRIER\b|\bWT_PAUSE\b|\bWT_BARRIER\b|\b__wt_atomic_', "hardware.h"),
    (r'\bF_CLR\b|\bF_ISSET\b|\bF_SET\b|\bWT_MIN\b|\bWT_MAX\b|\bWT_UNUSED\b|\bWT_MILLION\b'
     r'|\bWT_BILLION\b|\bWT_KB\b|\bWT_MB\b|\bWT_GB\b|\bWT_TB\b|\bWT_PTRDIFF\b|\bWT_CLOCKDIFF_\w+\b'
     r'|\bWT_CLOCK\b|\bWT_SIZE_CHECK_PACK\b|\bWT_STORE_SIZE\b', "misc.h"),
    (r'\bWT_DECL_RET\b|\bWT_RET\b|\bWT_ERR\b|\bWT_ASSERT\b|\bWT_ASSERT_ALWAYS\b|\bWT_TRET\b'
     r'|\bWT_ERR_MSG\b|\bWT_RET_MSG\b|\bWT_PANIC_RET\b|\bWT_PREFIX_MATCH\b|\bWT_STRING_MATCH\b', "error.h"),
    (r'\bWT_SPINLOCK\b|\bWT_CONDVAR\b|\bWT_RWLOCK\b', "mutex.h"),
    (r'\bWT_VERBOSE_\w+\b|\b__wt_verbose\b', "verbose.h"),
    (r'\bWT_STAT_CONN_\w+\b|\bWT_STAT_DSRC_\w+\b|\bWT_STAT_\w+_DECRV?\b|\bWT_STAT_\w+_INCRV?\b'
     r'|\bWT_STAT_\w+_SET\b', "stat.h"),
    (r'\bwt_timestamp_t\b|\bWT_TIME_WINDOW\b|\bWT_TIME_AGGREGATE\b', "timestamp.h"),
    (r'\bwt_off_t\b|\bWT_SYSCALL\b|\bWT_SYSCALL_RETRY\b|\bWT_FH\b|\bWT_FSTREAM\b', "os.h"),
    (r'\bwt_thread_t\b|\bWT_THREAD_CALLBACK\b', "posix.h"),
    (r'\bWT_FUTEX_\w+\b', "futex.h"),
    (r'\bCURSOR_API_CALL\b|\bWT_VERIFY_OPAQUE_POINTER\b', "api.h"),
    (r'\bWT_ASSERT_SPINLOCK_OWNED\b', "mutex_inline.h"),
    (r'\bWT_CONF_SIZING_NONE\b|\bWT_CONF_DEFAULT_VALUE_SHORTCUT\b', "conf.h"),
    (r'\bWT_CONFIG_ITEM_STATIC_INIT\b', "config.h"),
    (r'\b__bit_alloc\b|\b__bit_test\b|\b__bit_set\b|\b__bit_clear\b'
     r'|\b__bit_nclr\b|\b__bit_nset\b|\b__bit_ffc\b|\b__bit_ffs\b', "bitstring_inline.h"),
    (r'\b__wt_spin_lock_track\b|\b__wt_spin_lock\b|\b__wt_spin_unlock\b|\b__wt_spin_trylock\b'
     r'|\b__wt_spin_init\b|\b__wt_spin_destroy\b|\b__wt_spin_locked\b|\b__wt_spin_owned\b'
     r'|\bWT_ASSERT_SPINLOCK_OWNED\b|\bWT_SPIN_INIT_TRACKED\b|\bWT_SPIN_INIT_SESSION_TRACKED\b', "mutex_inline.h"),
    (r'\b__wt_cache_full\b|\b__wt_cache_bytes_inuse\b|\b__wt_cache_dirty_inuse\b'
     r'|\b__wt_session_can_wait\b|\b__wt_cache_pages_inuse\b', "cache_inline.h"),
    (r'\b__wt_struct_packv\b|\b__wt_struct_sizev\b|\b__wt_struct_unpackv\b'
     r'|\b__wt_struct_pack\b|\b__wt_struct_size\b|\b__wt_struct_unpack\b', "packing_inline.h"),
    (r'\b__wt_conf_gets_def_func\b|\b__wt_conf_get_compiled\b|\b__wt_conf_is_compiled\b'
     r'|\b__wt_conf_check_one\b|\b__wt_conf_parse_hex\b', "conf_inline.h"),
    (r'\bWT_VERIFY_OPAQUE_POINTER\b', "verify_build.h"),
    (r'\bCURSOR_API_CALL\b|\bCURSOR_API_CALL_PREPARE_ALLOWED\b'
     r'|\bAPI_SESSION_INIT\b|\bAPI_CALL\b|\bAPI_END_RET\b', "api.h"),
    (r'\b__cursor_needkey\b|\b__cursor_needvalue\b', "cursor_inline.h"),
    (r'\bWT_PACK\b|\bWT_DECL_PACK\b|\bWT_DECL_PACK_VALUE\b'
     r'|\b__pack_init\b|\b__pack_next\b|\b__pack_write\b|\b__pack_size\b'
     r'|\b__unpack_read\b|\bWT_PACK_GET\b|\bWT_UNPACK_PUT\b', "packing_inline.h"),
    (r'\bWT_REF_GET_STATE\b', "ref_inline.h"),
    (r'\b__wt_vsnprintf_len_set\b', "misc_inline.h"),
    (r'\bWT_WITH_LOCK_WAIT\b|\bWT_WITHOUT_LOCKS\b|\bWT_WITH_SCHEMA_LOCK\b'
     r'|\bWT_WITH_CHECKPOINT_LOCK\b|\bWT_WITH_TABLE_LOCK\b|\bWT_WITH_HANDLE_LIST_LOCK\b', "schema.h"),
    (r'\bWT_CELL_ADDR_DEL\b|\bWT_CELL_KEY\b(?!_)', "cell.h"),
    (r'\b__wt_live_restore_server_\w+\b|\b__wt_live_restore_metadata_to_fh\b', "live_restore.h"),
    (r'\bWT_STAT_USECS_HIST_INCR_FUNC\b', "stat.h"),
    (r'\bTAILQ_\w+\b|\bSTAILQ_\w+\b|\bSIMPLEQ_\w+\b', "queue.h"),
    (r'\b__wt_bswap\w+\b|\bWT_SWAP_BYTE\b', "swap.h"),
    (r'\bWT_BLOCK\b|\bWT_BM\b|\bWT_BLOCK_CKPT\b|\bWT_BLOCK_DESC\b|\bWT_BLOCK_HEADER\b', "block.h"),
    (r'\bWT_PAGE\b|\bWT_REF\b|\bWT_INSERT\b|\bWT_UPDATE\b|\bWT_ROW\b', "btmem.h"),
    (r'\bWT_BTREE\b(?!_)', "btree.h"),
    (r'\bWT_CELL\b|\bWT_CELL_UNPACK', "cell.h"),
    (r'\bWT_CONFIG\b(?!_ITEM)|\bWT_CONFIG_CHECK\b|\bWT_CONFIG_ENTRY\b', "config.h"),
    (r'\bWT_CONF_ID_\w+\b|\bWT_CONF_KEY_\w+\b', "conf_keys.h"),
    (r'\bWT_TABLE\b|\bWT_COLGROUP\b|\bWT_INDEX\b', "schema.h"),
    (r'\bWT_META_\w+\b|\bWT_METAFILE\w*\b', "meta.h"),
    (r'\bWT_TXN\b|\bWT_TXN_GLOBAL\b|\bWT_TXN_OP\b', "txn.h"),
    (r'\bWT_DATA_HANDLE\b', "dhandle.h"),
    (r'\bWT_DLH\b', "dlh.h"),
    (r'\bWT_JSON\b', "json.h"),
    (r'\bWT_HASH_MAP\b', "hash_map.h"),
    (r'\bWT_GENERATION_\w+\b', "generation.h"),
    (r'\bWT_HAZARD\b', "hazard.h"),
    (r'\bWT_CAPACITY\b|\bWT_THROTTLE_TYPE\b', "capacity.h"),
    (r'\bWT_COMPACT_STATE\b|\bWT_BACKGROUND_COMPACT\b', "compact.h"),
    (r'\bWT_OPTRACK_\w+\b', "optrack.h"),
    (r'\bWT_CRYPT_HEADER\b', "crypt_header.h"),
    (r'\bWT_VERSION\b(?!\()', "version.h"),
    (r'\bWT_TRUNCATE_INFO\b', "truncate.h"),
    (r'\bWT_ROLLBACK_TO_STABLE\b', "rollback_to_stable.h"),
    # Inline headers
    (r'\b__wt_buf_set\b|\b__wt_buf_grow\b|\b__wt_buf_init\b|\b__wt_buf_free\b'
     r'|\b__wt_scr_alloc\b|\b__wt_scr_free\b', "buf_inline.h"),
    (r'\b__wt_spin_lock\b|\b__wt_spin_unlock\b|\b__wt_spin_trylock\b'
     r'|\b__wt_cond_wait\b|\b__wt_cond_signal\b', "mutex_inline.h"),
    (r'\b__wt_snprintf\b|\b__wt_vsnprintf\b|\b__wt_safe_sub\b|\b__wt_safe_add\b', "misc_inline.h"),
    (r'\b__wt_close\b|\b__wt_open\b', "os_fhandle_inline.h"),
    (r'\b__wt_fs_directory_list\b|\b__wt_fs_exist\b|\b__wt_fs_remove\b|\b__wt_fs_rename\b'
     r'|\b__wt_fs_size\b', "os_fs_inline.h"),
    (r'\b__wt_fclose\b|\b__wt_fopen\b|\b__wt_fprintf\b|\b__wt_sync_and_rename\b', "os_fstream_inline.h"),
    (r'\b__wt_strnlen\b', "str_inline.h"),
    (r'\b__wt_isalnum\b|\b__wt_isdigit\b|\b__wt_isprint\b|\b__wt_isspace\b|\b__wt_tolower\b', "ctype_inline.h"),
]

PUBLIC_API_PATTERNS = [
    r'\bWT_SESSION\b(?!_IMPL)',
    r'\bWT_CONNECTION\b(?!_IMPL|_STATS)',
    r'\bWT_CURSOR\b(?!_BTREE|_BACKUP|_BULK|_HS|_DUMP|_INDEX|_TABLE|_STAT|_METADATA|_CONFIG|_DATA_SOURCE|_VERSION|_PREPARE_DISCOVERED|_LAYERED|_BOUNDS_STATE|_LOG)',
    r'\bWT_ITEM\b',
    r'\bWT_EVENT_HANDLER\b',
    r'\bWT_FILE_SYSTEM\b(?!_)',
    r'\bWT_FILE_HANDLE\b(?!_INMEM|_POSIX|_WIN)',
    r'\bWT_EXTENSION_API\b',
    r'\bWT_MODIFY\b',
    r'\bWT_CONFIG_ITEM\b',
]

INCLUDE_ORDER = [
    "wiredtiger_config.h", "wiredtiger_ext.h",
    "wt_system.h", "wt_compiler.h", "wt_fwd.h",
    "hardware.h", "swap.h", "queue.h", "posix.h", "os_windows.h",
    "misc.h", "tsan_suppress.h", "futex.h", "os.h", "optrack.h", "crypt_header.h",
    "dlh.h", "json.h", "bitstring.h", "compact.h", "generation.h", "hash_map.h",
    "hazard.h", "version.h", "meta.h", "capacity.h", "truncate.h", "rollback_to_stable.h",
    "mutex.h", "error.h", "verbose.h", "timestamp.h", "stat.h",
    "dhandle.h", "config.h", "conf_keys.h", "conf.h", "thread_group.h",
    "btmem.h", "block.h", "block_cache.h", "block_chunkcache.h", "cell.h",
    "schema.h", "txn.h", "api.h",
    "btree.h", "cursor.h", "cache.h", "tiered.h", "session.h",
    "connection.h",
    "../evict/evict.h", "../checkpoint/checkpoint.h", "../log/log.h",
    "../reconcile/reconcile.h", "../live_restore/live_restore.h",
    "extern_noninline.h",
    "extern_posix.h", "extern_win.h", "extern_linux.h", "extern_darwin.h",
    "verify_build.h",
    "cache_inline.h", "../evict/evict_inline.h", "ctype_inline.h",
    "intpack_inline.h", "int4bitpack_inline.h", "misc_inline.h",
    "generation_inline.h", "buf_inline.h", "ref_inline.h", "timestamp_inline.h",
    "cell_inline.h", "mutex_inline.h", "txn_inline.h",
    "bitstring_inline.h", "block_inline.h", "btree_inline.h", "btree_cmp_inline.h",
    "column_inline.h", "conf_inline.h", "cursor_inline.h", "../log/log_inline.h",
    "modify_inline.h", "os_fhandle_inline.h", "os_fs_inline.h", "os_fstream_inline.h",
    "packing_inline.h", "serial_inline.h", "str_inline.h", "time_inline.h",
]


SKIP_FILES = {
    "src/btree/bt_misc.c",
    "src/conf/conf_get.c",
    "src/block_disagg/block_disagg_read.c",
    "src/session/session_helper.c",
    "src/session/session_dhandle.c",
    "src/support/modify.c",
    "src/live_restore/live_restore_state.c",
    "src/meta/meta_table.c",
    "src/support/global.c",
    "src/support/json.c",
    "src/rollback_to_stable/rts_api.c",
    "src/conn/conn_page_history.c",
    "src/conn/conn_reconfig.c",
    "src/cursor/cur_backup.c",
    "src/meta/meta_turtle.c",
    "src/history/hs_conn.c",
}


def analyze_file(filepath):
    """Analyze a .c file and determine if it can be migrated and what it needs."""
    with open(filepath) as f:
        content = f.read()

    if '#include "wt_internal.h"' not in content:
        return None, "not using wt_internal.h"

    relpath = os.path.relpath(filepath, TOP_DIR)
    if relpath in SKIP_FILES:
        return None, "explicitly excluded (complex transitive deps)"

    needed = set()
    needed.add("wiredtiger_config.h")
    needed.add("wiredtiger_ext.h")
    needed.add("wt_system.h")
    needed.add("wt_compiler.h")
    needed.add("wt_fwd.h")

    for pattern, header in SYMBOL_HEADERS:
        if re.search(pattern, content):
            needed.add(header)

    # api.h macros expand to calls from these leaf inline headers at the call site
    COMPANION_INCLUDES = {
        "api.h": {"mutex_inline.h", "time_inline.h"},
        "schema.h": {"mutex_inline.h"},
    }
    for hdr, companions in COMPANION_INCLUDES.items():
        if hdr in needed:
            needed |= companions

    # Check if file uses session/connection types
    needs_session = False
    for pat in SESSION_PATTERNS:
        if re.search(pat, content):
            needed.add("session.h")
            needs_session = True
            break
    for pat in CONNECTION_PATTERNS:
        if re.search(pat, content):
            needed.add("connection.h")
            break
    # session.h users almost always need connection.h transitively
    # (stat.h macros, S2C, etc.)
    if needs_session:
        needed.add("connection.h")

    # If it calls __wt_* functions, it needs extern declarations.
    has_wt_funcs = bool(EXTERN_FN_PATTERN.search(content))
    if has_wt_funcs:
        needed.add("extern_noninline.h")

    # Headers whose macros expand to non-leaf inline function calls.
    HEAVYWEIGHT_HEADERS = {
        "live_restore.h",
    }
    heavy = needed & HEAVYWEIGHT_HEADERS
    if heavy:
        return None, f"needs heavyweight headers: {', '.join(sorted(heavy))}"

    # Check for inline header dependencies
    # If a header is a leaf (self-contained), keep it. Otherwise skip the file.
    inline_headers = {h for h in needed if h.endswith('_inline.h')}
    non_leaf_inline = inline_headers - LEAF_INLINE_HEADERS
    if non_leaf_inline:
        return None, f"needs non-leaf inline headers: {', '.join(sorted(non_leaf_inline))}"

    # Check for inline function calls not covered by SYMBOL_HEADERS.
    # If the function is in a leaf inline header, add it. Otherwise skip.
    for name in INLINE_FUNCTION_NAMES:
        if re.search(r'\b' + re.escape(name) + r'\b', content):
            if name in LEAF_INLINE_FUNCTIONS:
                needed.add(LEAF_INLINE_FUNCTIONS[name])
            else:
                return None, f"calls non-leaf inline function: {name}"

    return needed, "ok"


def sort_includes(headers):
    order_map = {h: i for i, h in enumerate(INCLUDE_ORDER)}
    return sorted(headers, key=lambda h: order_map.get(h, 999))


def generate_include_block(headers, is_posix=False, is_win=False, is_linux=False, is_darwin=False):
    sorted_hdrs = sort_includes(headers)
    lines = []
    for h in sorted_hdrs:
        if h == "posix.h" and not is_posix:
            lines.append('#ifndef _WIN32')
            lines.append(f'#include "{h}"')
            lines.append('#endif')
        elif h == "os_windows.h" and not is_win:
            lines.append('#ifdef _WIN32')
            lines.append(f'#include "{h}"')
            lines.append('#endif')
        elif h in ("extern_linux.h", "extern_darwin.h"):
            continue
        else:
            lines.append(f'#include "{h}"')

    # Add platform extern headers at the end
    if is_posix:
        lines.append('#include "extern_posix.h"')
        if is_linux:
            lines.append('#include "extern_linux.h"')
        elif is_darwin:
            lines.append('#include "extern_darwin.h"')
    elif is_win:
        lines.append('#include "extern_win.h"')
    else:
        # Cross-platform file
        lines.append('#ifdef _WIN32')
        lines.append('#include "extern_win.h"')
        lines.append('#else')
        lines.append('#include "extern_posix.h"')
        lines.append('#ifdef __linux__')
        lines.append('#include "extern_linux.h"')
        lines.append('#elif __APPLE__')
        lines.append('#include "extern_darwin.h"')
        lines.append('#endif')
        lines.append('#endif')

    return '\n'.join(lines)


def migrate_file(filepath, dry_run=False):
    needed, reason = analyze_file(filepath)
    if needed is None:
        return False, reason

    with open(filepath) as f:
        content = f.read()

    is_posix = '/os_posix/' in filepath
    is_win = '/os_win/' in filepath
    is_linux = '/os_linux/' in filepath
    is_darwin = '/os_darwin/' in filepath

    include_block = generate_include_block(
        needed, is_posix=is_posix or is_linux or is_darwin,
        is_win=is_win, is_linux=is_linux, is_darwin=is_darwin)

    new_content = content.replace('#include "wt_internal.h"', include_block)

    if dry_run:
        print(f"\n=== {os.path.relpath(filepath, TOP_DIR)} ===")
        print(f"Headers: {len(needed)}")
        for h in sort_includes(needed):
            print(f"  {h}")
        return True, f"{len(needed)} headers"

    with open(filepath, 'w') as f:
        f.write(new_content)

    return True, f"{len(needed)} headers"


def find_c_files(module_path):
    full_path = os.path.join(TOP_DIR, module_path)
    if not os.path.isdir(full_path):
        return []
    return sorted(os.path.join(full_path, fn) for fn in os.listdir(full_path) if fn.endswith('.c'))


def main():
    parser = argparse.ArgumentParser(description="Migrate .c files to specific includes")
    parser.add_argument("--module", action="append", help="Module directory")
    parser.add_argument("--all", action="store_true", help="Migrate all modules")
    parser.add_argument("--dry-run", action="store_true", help="Preview only")
    parser.add_argument("--file", help="Migrate single file")
    args = parser.parse_args()

    if args.file:
        filepath = os.path.join(TOP_DIR, args.file) if not os.path.isabs(args.file) else args.file
        ok, msg = migrate_file(filepath, dry_run=args.dry_run)
        action = "Would migrate" if args.dry_run else "Migrated"
        print(f"{action if ok else 'Skipped'}: {args.file} ({msg})")
        return

    modules = args.module or []
    if args.all:
        modules = [
            "src/checksum", "src/os_posix", "src/os_win", "src/os_darwin", "src/os_linux",
            "src/os_common", "src/support", "src/packing", "src/config", "src/conf",
            "src/block", "src/block_cache", "src/block_disagg",
            "src/meta", "src/schema", "src/log", "src/live_restore",
            "src/cursor", "src/btree", "src/history", "src/prepared_discover",
            "src/evict", "src/txn", "src/checkpoint",
            "src/reconcile", "src/rollback_to_stable", "src/session", "src/conn", "src/tiered",
        ]

    # Reload inline function names (they may have changed since prototypes were regenerated)
    global INLINE_FUNCTION_NAMES, LEAF_INLINE_FUNCTIONS
    INLINE_FUNCTION_NAMES = _load_inline_function_names()
    LEAF_INLINE_FUNCTIONS = _load_leaf_inline_function_names()

    total = 0
    migrated = 0
    skipped_inline = 0
    skipped_other = 0
    for mod in modules:
        c_files = find_c_files(mod)
        for fpath in c_files:
            total += 1
            rel = os.path.relpath(fpath, TOP_DIR)
            ok, msg = migrate_file(fpath, dry_run=args.dry_run)
            if ok:
                migrated += 1
                if not args.dry_run:
                    print(f"Migrated: {rel} ({msg})")
            else:
                if "inline" in msg:
                    skipped_inline += 1
                else:
                    skipped_other += 1
                if not args.dry_run:
                    print(f"Skipped:  {rel} ({msg})")

    print(f"\n{'Preview' if args.dry_run else 'Result'}: {migrated}/{total} files migrated, "
          f"{skipped_inline} skipped (inline deps), {skipped_other} skipped (other)")


if __name__ == "__main__":
    main()
