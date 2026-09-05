#pragma once

// CUSTOM (TeTeHacko): channel-message deny list -- not part of upstream
// simple_repeater. Included at the bottom of MyMesh.cpp, after ChannelCommon.h.
//
// WHAT THIS IS (and is not)
//
// A repeater that knows a channel's PSK (FILTER_CHANNEL_PSKS, see
// ChannelCommon.h) decrypts each GRP_TXT on it and refuses to FORWARD messages
// whose "<sender>: " prefix is on a runtime deny list. Born 5. 9. 2026, when a
// runaway AI bot looped an 11-part Gemini quota dump onto #meshcore for hours
// and every repeater dutifully re-aired it.
//
//   * The sender name is just text inside the encrypted payload -- GRP_TXT
//     carries NO identity (only adverts do). Anyone can rename around this.
//     It is containment of a runaway/rude node, not security. (For the
//     content-neutral automatic layer, see FairnessLimiter.h.)
//   * Only forwarding by THIS node is affected. Reception, rxlog/pktfeed
//     capture (CoreScope feed) and the rest of the mesh are untouched.
//   * DIRECT-routed packets are not seen here (their forward is decided before
//     any decrypt, Mesh.cpp:94) -- channel messages flood, so in practice this
//     covers them.
//   * Upstream would not take this (privacy/no-censorship principles, see
//     issue #169 discussion) -- fork-only, do not upstream.
//
// The deny list lives in _prefs.filter_deny (comma-separated exact names,
// case-insensitive) so the stock savePrefs()/loadPrefs() machinery persists
// it, and it is editable at runtime over RF admin: an incident response is
// `filter add <name>`, not a mast reflash.

#ifdef FILTER_CHANNEL_PSKS

bool MyMesh::filterDenied(const char* sender) const {
  if (_prefs.filter_deny[0] == 0) return false;
  return chan_list_has(_prefs.filter_deny, sender);
}

// `filter` / `filter list` -- show state; `filter add|del <name>` -- edit;
// `filter clear` -- empty the list. Names may contain spaces (everything after
// the subcommand is the name); commas are the storage separator so they are
// rejected. Matching is exact and case-insensitive (chan_list_has).
bool MyMesh::filterHandleCommand(const char* command, char* reply, int reply_max) {
  if (memcmp(command, "filter", 6) != 0 || (command[6] != 0 && command[6] != ' ')) return false;

  if (memcmp(&command[6], " add ", 5) == 0) {
    const char* name = &command[11];
    int rc = chan_list_add(_prefs.filter_deny, sizeof(_prefs.filter_deny), name, strlen(name));
    if (rc == CHAN_LIST_OK) {
      savePrefs();
      sprintf(reply, "OK - deny: %s", _prefs.filter_deny);
    } else if (rc == CHAN_LIST_DUP) {
      strcpy(reply, "Err - already listed");
    } else if (rc == CHAN_LIST_FULL) {
      strcpy(reply, "Err - list full");
    } else {
      strcpy(reply, "Err - bad name (empty, too long, or contains ',')");
    }
    return true;
  }

  if (memcmp(&command[6], " del ", 5) == 0) {
    if (chan_list_del(_prefs.filter_deny, &command[11])) {
      savePrefs();
      strcpy(reply, "OK");
    } else {
      strcpy(reply, "Err - not listed");
    }
    return true;
  }

  if (strcmp(&command[6], " clear") == 0) {
    _prefs.filter_deny[0] = 0;
    savePrefs();
    strcpy(reply, "OK - deny list cleared");
    return true;
  }

  if (command[6] == 0 || strcmp(&command[6], " list") == 0) {
    snprintf(reply, reply_max, "filter: %d chan, dropped %u, deny: %s",
             (int)num_known_channels, filter_dropped,
             _prefs.filter_deny[0] ? _prefs.filter_deny : "(nikdo)");
    return true;
  }

  strcpy(reply, "Err - filter [add|del <name>|list|clear]");
  return true;
}

#endif   // FILTER_CHANNEL_PSKS
