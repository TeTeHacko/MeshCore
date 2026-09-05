#pragma once

// CUSTOM (TeTeHacko): pure string helpers for the channel modules
// (ChannelCommon.h, ChannelFilter.h, ChannelBot.h). No MyMesh, no Arduino --
// deliberately, so test/test_chan_list can compile them on the native env.

#include <ctype.h>
#include <string.h>

// Longest node name an entry may hold. MeshCore node names are 32 bytes
// (NodePrefs.node_name), the margin is for UTF-8 names people paste with
// trailing decoration.
#define CHAN_NAME_MAX     40

#define CHAN_LIST_OK       0
#define CHAN_LIST_BAD_NAME -1   // empty, too long, or contains the ',' separator
#define CHAN_LIST_DUP      -2
#define CHAN_LIST_FULL     -3

static int chan_hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exact, case-insensitive membership in a comma-separated list. Deliberately
// NOT a substring search: an ignore/deny list matching on substrings would be
// a trap (a node called "TTH" silencing "TTH-L1"). Entries are trimmed of
// surrounding spaces; case folding is ASCII-only, so UTF-8 names match
// byte-exactly (good enough -- nobody case-shifts emoji).
static bool chan_list_has(const char* list, const char* word) {
  const char* seg = list;
  while (*seg) {
    const char* comma = strchr(seg, ',');
    int len = comma ? (int)(comma - seg) : (int)strlen(seg);
    while (len > 0 && *seg == ' ') { seg++; len--; }
    while (len > 0 && seg[len - 1] == ' ') len--;
    if (len > 0 && (int)strlen(word) == len) {
      int i = 0;
      while (i < len && tolower((unsigned char)word[i]) == tolower((unsigned char)seg[i])) i++;
      if (i == len) return true;
    }
    if (!comma) break;
    seg = comma + 1;
  }
  return false;
}

// Append name[0..name_len) to the comma-separated `list` (buffer of `cap`
// bytes, NUL-terminated). Leading/trailing spaces are trimmed off the name
// first (RF-padded commands happen).
static int chan_list_add(char* list, int cap, const char* name, int name_len) {
  while (name_len > 0 && *name == ' ') { name++; name_len--; }
  while (name_len > 0 && name[name_len - 1] == ' ') name_len--;
  if (name_len <= 0 || name_len >= CHAN_NAME_MAX) return CHAN_LIST_BAD_NAME;
  if (memchr(name, ',', name_len) != NULL) return CHAN_LIST_BAD_NAME;   // ',' is the separator

  char tmp[CHAN_NAME_MAX];
  memcpy(tmp, name, name_len);
  tmp[name_len] = 0;
  if (chan_list_has(list, tmp)) return CHAN_LIST_DUP;

  int cur = strlen(list);
  if (cur + (cur > 0 ? 1 : 0) + name_len + 1 > cap) return CHAN_LIST_FULL;
  if (cur > 0) list[cur++] = ',';
  memcpy(&list[cur], tmp, name_len + 1);
  return CHAN_LIST_OK;
}

// Remove the (case-insensitively) matching entry. Returns false when absent.
static bool chan_list_del(char* list, const char* name) {
  while (*name == ' ') name++;
  int name_len = strlen(name);
  while (name_len > 0 && name[name_len - 1] == ' ') name_len--;   // trim trailing too (RF pads)
  char out[512];   // callers' lists are far smaller (filter_deny is 128 B)
  int out_len = 0;
  bool removed = false;
  const char* seg = list;
  int list_len = strlen(list);
  if (list_len >= (int)sizeof(out)) return false;   // defensive, never expected

  while (*seg) {
    const char* comma = strchr(seg, ',');
    int len = comma ? (int)(comma - seg) : (int)strlen(seg);
    const char* e = seg;
    int elen = len;
    while (elen > 0 && *e == ' ') { e++; elen--; }
    while (elen > 0 && e[elen - 1] == ' ') elen--;
    bool match = false;
    if (name_len == elen) {
      int i = 0;
      while (i < elen && tolower((unsigned char)name[i]) == tolower((unsigned char)e[i])) i++;
      match = (i == elen);
    }
    if (match) {
      removed = true;
    } else if (len > 0) {
      if (out_len > 0) out[out_len++] = ',';
      memcpy(&out[out_len], seg, len);
      out_len += len;
    }
    if (!comma) break;
    seg = comma + 1;
  }
  if (removed) {
    out[out_len] = 0;
    memcpy(list, out, out_len + 1);
  }
  return removed;
}
