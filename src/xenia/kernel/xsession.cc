/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Emulator. All rights reserved.                        *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <ranges>

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/XLiveAPI.h"
#include "xenia/kernel/xsession.h"

DECLARE_bool(upnp);

namespace xe {
namespace kernel {

XSession::XSession(KernelState* kernel_state)
    : XObject(kernel_state, Type::Session) {
  session_id_ = -1;
}

X_STATUS XSession::Initialize() {
  auto native_object = CreateNative(sizeof(X_KSESSION));
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }

  auto guest_object = reinterpret_cast<X_KSESSION*>(native_object);
  guest_object->handle = handle();
  // Based on what is in XAM it seems like size of this object is only 4 bytes.
  return X_STATUS_SUCCESS;
}

X_RESULT XSession::CreateSession(uint32_t user_index, uint8_t public_slots,
                                 uint8_t private_slots, uint32_t flags,
                                 uint32_t session_info_ptr,
                                 uint32_t nonce_ptr) {
  if (IsCreated()) {
    // Todo: Find proper code!
    return X_ERROR_FUNCTION_FAILED;
  }

  const xam::UserProfile* user_profile =
      kernel_state_->xam_state()->GetUserProfile(user_index);
  if (!user_profile) {
    return X_ERROR_FUNCTION_FAILED;
  }

  // Session type is ranked but ARBITRATION flag isn't set
  if (GetGameTypeValue(user_profile->xuid()) == X_CONTEXT_GAME_TYPE_RANKED &&
      !(flags & ARBITRATION)) {
    return X_ONLINE_E_SESSION_REQUIRES_ARBITRATION;
  }

  // 58410889
  // If a session requires online features but we're offline then we must fail.
  // e.g. Trying to create a SINGLEPLAYER_WITH_STATS session while not connected
  // to live.
  if (HasXboxLiveFeatureFlags() &&
      user_profile->signin_state() !=
          xam::X_USER_SIGNIN_STATE::SignedInToLive) {
    return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
  }

  XSESSION_INFO* SessionInfo_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(
          session_info_ptr);

  GenerateIdentityExchangeKey(&SessionInfo_ptr->keyExchangeKey);
  PrintSessionType((SessionFlags)flags);

  uint64_t* Nonce_ptr =
      kernel_state_->memory()->TranslateVirtual<uint64_t*>(nonce_ptr);

  local_details_.UserIndexHost = XUserIndexNone;

  // Set early so utility functions can check flags
  local_details_.Flags = flags;

  // CSGO only uses STATS flag to create a session to POST stats pre round.
  // Minecraft and Portal 2 use flags HOST + STATS.
  //
  // Hexic creates a session with SINGLEPLAYER_WITH_STATS (without HOST bit)
  // with contexts
  //
  // Create presence sessions?
  // - Create when joining a session
  // - Explicitly create a presence session (Frogger without HOST bit)
  // Based on Presence flag set?

  // 584107FB expects offline session creation by specifying 0 (a session
  // without Xbox Live features) to succeed while offline for local multiplayer.
  //
  // 58410889 expects SINGLEPLAYER_WITH_STATS session creation failure while
  // offline.

  if (flags == STATS) {
    CreateStatsSession(SessionInfo_ptr, Nonce_ptr, user_index, public_slots,
                       private_slots, flags);
  } else if (HasSessionFlag((SessionFlags)flags, HOST) ||
             flags == SINGLEPLAYER_WITH_STATS || HasOfflineFlags()) {
    CreateHostSession(SessionInfo_ptr, Nonce_ptr, user_index, public_slots,
                      private_slots, flags);
  } else {
    JoinExistingSession(SessionInfo_ptr);
  }

  local_details_.GameType = GetGameTypeValue(user_profile->xuid());
  local_details_.GameMode = GetGameModeValue(user_profile->xuid());
  local_details_.MaxPublicSlots = public_slots;
  local_details_.MaxPrivateSlots = private_slots;
  local_details_.AvailablePublicSlots = public_slots;
  local_details_.AvailablePrivateSlots = private_slots;
  local_details_.ActualMemberCount = 0;
  local_details_.ReturnedMemberCount = 0;
  local_details_.eState = XSESSION_STATE::LOBBY;
  local_details_.Nonce = *Nonce_ptr;
  local_details_.sessionInfo = *SessionInfo_ptr;
  local_details_.xnkidArbitration = XNKID{};
  local_details_.SessionMembers_ptr = 0;

  state_ |= STATE_FLAGS_CREATED;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::CreateHostSession(XSESSION_INFO* session_info,
                                     uint64_t* nonce_ptr, uint8_t user_index,
                                     uint8_t public_slots,
                                     uint8_t private_slots, uint32_t flags) {
  state_ |= STATE_FLAGS_HOST;

  local_details_.UserIndexHost = user_index;

  if (!cvars::upnp) {
    XELOGI("Hosting while UPnP is disabled!");
  }

  std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist(0, -1);
  *nonce_ptr = dist(rd);

  XGI_SESSION_CREATE session_data = {};
  session_data.user_index = user_index;
  session_data.num_slots_public = public_slots;
  session_data.num_slots_private = private_slots;
  session_data.flags = flags;

  const uint64_t systemlink_id = XLiveAPI::systemlink_id;

  if (HasOfflineFlags()) {
    XELOGI("Creating an offline session");

    // what session ID mask should be used here?
    session_id_ = GenerateSessionId(XNKID_SYSTEM_LINK);

  } else if (HasSystemlinkFlags()) {
    XELOGI("Creating systemlink session");

    // If XNetRegisterKey did not register key then we must register it here
    if (systemlink_id) {
      session_id_ = systemlink_id;
    } else {
      session_id_ = GenerateSessionId(XNKID_SYSTEM_LINK);
      XLiveAPI::systemlink_id = session_id_;
    }
  } else if (HasXboxLiveFeatureFlags()) {
    XELOGI("Creating xbox live session");
    session_id_ = GenerateSessionId(XNKID_ONLINE);

    // 58410821 adds properties after session creation
    // Properties are ad-hoc therefore should be updated on backend, only
    // update if value changed to reduce POST requests.
    XLiveAPI::XSessionCreate(session_id_, &session_data);
    XLiveAPI::SessionPropertiesSet(session_id_, session_data.user_index);
  } else {
    assert_always();
  }

  XELOGI("Created session {:016X}", session_id_);

  IsValidXNKID(session_id_);

  Uint64toXNKID(session_id_, &session_info->sessionID);
  XLiveAPI::IpGetConsoleXnAddr(&session_info->hostAddress);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::CreateStatsSession(XSESSION_INFO* session_info,
                                      uint64_t* nonce_ptr, uint8_t user_index,
                                      uint8_t public_slots,
                                      uint8_t private_slots, uint32_t flags) {
  return CreateHostSession(session_info, nonce_ptr, user_index, public_slots,
                           private_slots, flags);
}

X_RESULT XSession::JoinExistingSession(XSESSION_INFO* session_info) {
  session_id_ = XNKIDtoUint64(&session_info->sessionID);
  XELOGI("Joining session {:016X}", session_id_);

  IsValidXNKID(session_id_);

  if (kernel::IsSystemlink(session_id_)) {
    XELOGI("Joining systemlink session");
    return X_ERROR_SUCCESS;
  } else if (kernel::IsOnlinePeer(session_id_)) {
    XELOGI("Joining xbox live session");
  } else {
    XELOGI("Joining unknown session type!");
    assert_always();
  }

  const std::unique_ptr<SessionObjectJSON> session =
      XLiveAPI::XSessionGet(session_id_);

  // Begin XNetRegisterKey?

  if (!session->HostAddress().empty()) {
    GetXnAddrFromSessionObject(session.get(), &session_info->hostAddress);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::DeleteSession(XGI_SESSION_STATE* state) {
  // Begin XNetUnregisterKey?

  state_ |= STATE_FLAGS_DELETED;

  if (IsHost() && HasXboxLiveFeatureFlags()) {
    XLiveAPI::DeleteSession(session_id_);
  }

  session_id_ = 0;

  // Multiple sessions cause issues
  // XLiveAPI::systemlink_id = session_id_;

  local_details_.eState = XSESSION_STATE::DELETED;
  // local_details_.sessionInfo.sessionID = XNKID{};
  return X_ERROR_SUCCESS;
}

// A member can be added by either local or remote, typically local members
// are joined via local but are often joined via remote - they're equivalent.
//
// If there are no private slots available then the member will occupy a
// public slot instead.
//
// TODO(Adrian):
// Add player to recent player list.
// Joining a offline session uses which XUID offline or online (flags = 0)
// Return correct error codes
X_RESULT XSession::JoinSession(XGI_SESSION_MANAGE* data) {
  const bool join_local = data->xuid_array_ptr == 0;

  std::string join_type =
      join_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote";

  XELOGI("{}({:08X}, {}, {:08X}, {:08X}, {:08X})", join_type,
         data->obj_ptr.get(), data->array_count.get(),
         data->xuid_array_ptr.get(), data->indices_array_ptr.get(),
         data->private_slots_array_ptr.get());

  std::unordered_map<uint64_t, bool> members{};

  const auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  const auto indices_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->indices_array_ptr);

  const auto private_slots_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->private_slots_array_ptr);

  for (uint32_t i = 0; i < data->array_count; i++) {
    XSESSION_MEMBER* member = new XSESSION_MEMBER();

    if (join_local) {
      const uint32_t user_index = static_cast<uint32_t>(indices_array[i]);

      if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
        return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
      }

      const auto user_profile =
          kernel_state()->xam_state()->GetUserProfile(user_index);
      const xe::be<uint64_t> xuid_online = user_profile->GetLogonXUID();

      assert_true(IsValidXUID(xuid_online));

      if (local_members_.count(xuid_online) ||
          remote_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member->OnlineXUID = xuid_online;
      member->UserIndex = user_index;

      local_details_.ActualMemberCount = std::min<int32_t>(
          XUserMaxUserCount, local_details_.ActualMemberCount + 1);
    } else {
      const xe::be<uint64_t> xuid_online = xuid_array[i];
      uint8_t user_index =
          kernel_state()->xam_state()->GetUserIndexAssignedToProfileFromXUID(
              xuid_online);

      if (user_index == XUserIndexAny) {
        user_index = XUserIndexNone;
      }

      assert_true(IsValidXUID(xuid_online));

      if (remote_members_.count(xuid_online) ||
          local_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member->OnlineXUID = xuid_online;
      member->UserIndex = user_index;

      const bool is_local_member =
          kernel_state()->xam_state()->IsUserSignedIn(xuid_online);

      if (is_local_member) {
        local_details_.ActualMemberCount = std::min<int32_t>(
            XUserMaxUserCount, local_details_.ActualMemberCount + 1);
      }
    }

    const bool is_private = private_slots_array[i];

    if (is_private && local_details_.AvailablePrivateSlots > 0) {
      member->SetPrivate();

      local_details_.AvailablePrivateSlots =
          std::max<int32_t>(0, local_details_.AvailablePrivateSlots - 1);
    } else {
      local_details_.AvailablePublicSlots =
          std::max<int32_t>(0, local_details_.AvailablePublicSlots - 1);
    }

    XELOGI("XUID: {:016X} - Occupying {} slot", member->OnlineXUID.get(),
           member->IsPrivate() ? "private" : "public");

    members[member->OnlineXUID] = member->IsPrivate();

    if (join_local) {
      local_members_.emplace(member->OnlineXUID, *member);
    } else {
      remote_members_.emplace(member->OnlineXUID, *member);
    }
  }

  local_details_.ReturnedMemberCount = GetMembersCount();

  if (!members.empty() && IsHost() && HasXboxLiveFeatureFlags()) {
    XLiveAPI::SessionJoinRemote(session_id_, members);
  } else if (!members.empty() && !HasOfflineFlags()) {
    // To improve XNetInAddrToXnAddr stability each members session id
    // must match host. This is a workaround and should be fixed properly.
    //
    // 545107D1 will fail to join sessions if session id doesn't match.

    const auto keys = std::views::keys(members);
    std::set<uint64_t> xuids{keys.begin(), keys.end()};

    XLiveAPI::SessionPreJoin(session_id_, xuids);
  }

  // XamUserAddRecentPlayer

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::LeaveSession(XGI_SESSION_MANAGE* data) {
  const bool leave_local = data->xuid_array_ptr == 0;

  std::string leave_type =
      leave_local ? "XGISessionLeaveLocal" : "XGISessionLeaveRemote";

  XELOGI("{}({:08X}, {}, {:08X}, {:08X})", leave_type, data->obj_ptr.get(),
         data->array_count.get(), data->xuid_array_ptr.get(),
         data->indices_array_ptr.get());

  // Server already knows slots types from joining so we only need to send
  // xuids.
  std::vector<xe::be<uint64_t>> xuids{};

  auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  auto indices_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
          data->indices_array_ptr);

  bool is_arbitrated = HasSessionFlag(
      static_cast<SessionFlags>((uint32_t)local_details_.Flags), ARBITRATION);

  const auto profile_manager = kernel_state()->xam_state()->profile_manager();

  for (uint32_t i = 0; i < data->array_count; i++) {
    XSESSION_MEMBER* member = new XSESSION_MEMBER();

    if (leave_local) {
      const uint32_t user_index = static_cast<uint32_t>(indices_array[i]);

      if (!kernel_state()->xam_state()->IsUserSignedIn(user_index)) {
        return X_ONLINE_E_SESSION_NOT_LOGGED_ON;
      }

      const auto user_profile =
          kernel_state()->xam_state()->GetUserProfile(user_index);
      const xe::be<uint64_t> xuid_online = user_profile->GetLogonXUID();

      assert_true(IsValidXUID(xuid_online));

      if (!local_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member = &local_members_[xuid_online];
    } else {
      const xe::be<uint64_t> xuid_online = xuid_array[i];

      assert_true(IsValidXUID(xuid_online));

      if (!remote_members_.count(xuid_online)) {
        return X_ERROR_SUCCESS;
      }

      member = &remote_members_[xuid_online];
    }

    if (member->IsPrivate()) {
      // Removing a private member but all members are removed
      assert_false(local_details_.AvailablePrivateSlots ==
                   local_details_.MaxPrivateSlots);

      local_details_.AvailablePrivateSlots =
          std::min<int32_t>(local_details_.MaxPrivateSlots,
                            local_details_.AvailablePrivateSlots + 1);
    } else {
      // Removing a public member but all members are removed
      assert_false(local_details_.AvailablePublicSlots ==
                   local_details_.MaxPublicSlots);

      local_details_.AvailablePublicSlots =
          std::min<int32_t>(local_details_.MaxPublicSlots,
                            local_details_.AvailablePublicSlots + 1);
    }

    // Keep arbitrated session members for stats reporting
    if (is_arbitrated) {
      member->SetZombie();
    }

    if (!member->IsZombie()) {
      bool removed = false;

      XELOGI("XUID: {:016X} - Leaving {} slot", member->OnlineXUID.get(),
             member->IsPrivate() ? "private" : "public");

      const xe::be<uint64_t> xuid_online = member->OnlineXUID;

      if (leave_local) {
        removed = local_members_.erase(xuid_online);
      } else {
        removed = remote_members_.erase(xuid_online);
      }

      assert_true(removed);

      if (removed) {
        xuids.push_back(xuid_online);

        const bool is_local_member =
            kernel_state()->xam_state()->IsUserSignedIn(xuid_online);

        if (is_local_member) {
          local_details_.ActualMemberCount =
              std::max<int32_t>(0, local_details_.ActualMemberCount - 1);
        }
      }
    }
  }

  local_details_.ReturnedMemberCount = GetMembersCount();

  if (!xuids.empty() && IsHost() && HasXboxLiveFeatureFlags()) {
    XLiveAPI::SessionLeaveRemote(session_id_, xuids);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ModifySession(XGI_SESSION_MODIFY* data) {
  XELOGI("Modifying session {:016X}", session_id_);

  XGI_SESSION_MODIFY modify = *data;

  if (IsValidModifyFlags(data->flags)) {
    PrintSessionType(static_cast<SessionFlags>((uint32_t)data->flags));

    local_details_.Flags = data->flags;
  } else {
    modify.flags = local_details_.Flags;
    XELOGI("{}: Invalid Flags!", __func__);
  }

  const uint32_t num_private_slots = std::max<int32_t>(
      0, local_details_.MaxPrivateSlots - local_details_.AvailablePrivateSlots);

  const uint32_t num_public_slots = std::max<int32_t>(
      0, local_details_.MaxPublicSlots - local_details_.AvailablePublicSlots);

  data->maxPrivateSlots = std::max<int32_t>(0, data->maxPrivateSlots);
  data->maxPublicSlots = std::max<int32_t>(0, data->maxPublicSlots);

  local_details_.MaxPrivateSlots = data->maxPrivateSlots;
  local_details_.MaxPublicSlots = data->maxPublicSlots;

  local_details_.AvailablePrivateSlots =
      std::max<int32_t>(0, local_details_.MaxPrivateSlots - num_private_slots);
  local_details_.AvailablePublicSlots =
      std::max<int32_t>(0, local_details_.MaxPublicSlots - num_public_slots);

  PrintSessionDetails();

  if (IsHost() && HasXboxLiveFeatureFlags()) {
    XLiveAPI::SessionModify(session_id_, &modify);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionDetails(XGI_SESSION_DETAILS* data) {
  // 4E4D085C checks ReturnedMemberCount when creating a session

  auto local_details_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(
          data->session_details_ptr);

  local_details_ptr->SessionMembers_ptr =
      kernel_state_->memory()->SystemHeapAlloc(sizeof(XSESSION_MEMBER) *
                                               GetMembersCount());

  local_details_.SessionMembers_ptr = local_details_ptr->SessionMembers_ptr;

  XSESSION_MEMBER* members_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_MEMBER*>(
          local_details_ptr->SessionMembers_ptr);

  uint32_t index = 0;

  for (auto const& [xuid, member] : local_members_) {
    members_ptr[index] = member;
    index++;
  }

  for (auto const& [xuid, member] : remote_members_) {
    members_ptr[index] = member;
    index++;
  }

  memcpy(local_details_ptr, &local_details_, sizeof(XSESSION_LOCAL_DETAILS));

  PrintSessionDetails();

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::MigrateHost(XGI_SESSION_MIGRATE* data) {
  auto SessionInfo_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(
          data->session_info_ptr);

  if (!XLiveAPI::upnp_handler->is_active()) {
    XELOGI("Migrating without UPnP");
    // return X_E_FAIL;
  }

  const auto result = XLiveAPI::XSessionMigration(session_id_, data);

  if (!result->SessionID_UInt()) {
    XELOGI("Session Migration Failed");

    // Returning X_E_FAIL will cause 5454082B to restart
    return X_E_FAIL;
  }

  if (data->user_index == XUserIndexNone) {
    XELOGI("Session migration we're not host!");
  }

  if (kernel_state()->xam_state()->IsUserSignedIn(data->user_index)) {
    // Update properties, what if they're changed after migration?
    XLiveAPI::SessionPropertiesSet(result->SessionID_UInt(), data->user_index);
  }

  memset(SessionInfo_ptr, 0, sizeof(XSESSION_INFO));

  Uint64toXNKID(result->SessionID_UInt(), &SessionInfo_ptr->sessionID);
  XLiveAPI::IpGetConsoleXnAddr(&SessionInfo_ptr->hostAddress);
  GenerateIdentityExchangeKey(&SessionInfo_ptr->keyExchangeKey);

  // Update session id to migrated session id
  session_id_ = result->SessionID_UInt();

  state_ |= STATE_FLAGS_HOST;
  state_ |= STATE_FLAGS_MIGRATED;

  local_details_.UserIndexHost = data->user_index;
  local_details_.sessionInfo = *SessionInfo_ptr;
  local_details_.xnkidArbitration = local_details_.sessionInfo.sessionID;

  return X_ERROR_SUCCESS;
}

// Server dependancy can be removed if we calculate remote machine id from
// remote mac address.
X_RESULT XSession::RegisterArbitration(XGI_SESSION_ARBITRATION* data) {
  XSESSION_REGISTRATION_RESULTS* results_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_REGISTRATION_RESULTS*>(
          data->results_ptr);

  const auto result = XLiveAPI::XSessionArbitration(session_id_);

  const uint32_t registrants_ptr =
      kernel_state_->memory()->SystemHeapAlloc(static_cast<uint32_t>(
          sizeof(XSESSION_REGISTRANT) * result->Machines().size()));

  results_ptr->registrants_count =
      static_cast<uint32_t>(result->Machines().size());
  results_ptr->registrants_ptr = registrants_ptr;

  XSESSION_REGISTRANT* registrants =
      kernel_state_->memory()->TranslateVirtual<XSESSION_REGISTRANT*>(
          registrants_ptr);

  for (uint8_t i = 0; i < result->Machines().size(); i++) {
    registrants[i].trustworthiness = 1;

    registrants[i].machine_id = result->Machines()[i].machine_id;
    registrants[i].num_users = result->Machines()[i].player_count;

    const uint32_t users_ptr = kernel_state_->memory()->SystemHeapAlloc(
        sizeof(uint64_t) * registrants[i].num_users);

    uint64_t* users_xuid_ptr =
        kernel_state_->memory()->TranslateVirtual<uint64_t*>(users_ptr);

    for (uint8_t j = 0; j < registrants[i].num_users; j++) {
      users_xuid_ptr[j] = result->Machines()[i].xuids[j];
    }

    registrants[i].users_ptr = users_ptr;
  }

  Uint64toXNKID(session_id_, &local_details_.xnkidArbitration);

  local_details_.eState = XSESSION_STATE::REGISTRATION;

  // Assert?
  // local_details_.Nonce = data->session_nonce;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ModifySkill(XGI_SESSION_MODIFYSKILL* data) {
  const auto xuid_array =
      kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
          data->xuid_array_ptr);

  for (uint32_t i = 0; i < data->array_count; i++) {
    const auto& xuid = xuid_array[i];

    XELOGI("ModifySkill XUID: {:016X}", xuid.get());
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::WriteStats(XGI_STATS_WRITE* data) {
  if (!HasSessionFlag(static_cast<SessionFlags>((uint32_t)local_details_.Flags),
                      STATS)) {
    XELOGW("Session does not support stats.");
    return X_ERROR_FUNCTION_FAILED;
  }

  if (local_details_.eState != XSESSION_STATE::INGAME) {
    XELOGW("Writing stats outside of gameplay.");
    return X_ERROR_FUNCTION_FAILED;
  }

  if (!data->num_views) {
    XELOGW("No leaderboard stats to write.");
    return X_ERROR_SUCCESS;
  }

  XLiveAPI::SessionWriteStats(session_id_, *data);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::StartSession(XGI_SESSION_STATE* state) {
  local_details_.eState = XSESSION_STATE::INGAME;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::EndSession(XGI_SESSION_STATE* state) {
  local_details_.eState = XSESSION_STATE::REPORTING;

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessions(KernelState* kernel_state,
                               XGI_SESSION_SEARCH* search_data,
                               uint32_t num_users) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size =
        sizeof(XSESSION_SEARCHRESULT) * search_data->num_results;
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  const auto sessions = XLiveAPI::SessionSearch(search_data, num_users);

  const uint32_t session_count = std::min<int32_t>(
      search_data->num_results, static_cast<uint32_t>(sessions.size()));

  const uint32_t session_ids =
      kernel_state->memory()->SystemHeapAlloc(session_count * sizeof(XNKID));

  XNKID* session_ids_ptr =
      kernel_state->memory()->TranslateVirtual<XNKID*>(session_ids);

  for (uint32_t i = 0; i < session_count; i++) {
    XNKID id = {};
    Uint64toXNKID(sessions.at(i)->SessionID_UInt(), &id);

    session_ids_ptr[i] = id;
  }

  GetSessionByIDs(kernel_state->memory(), session_ids_ptr, session_count,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  SEARCH_RESULTS* search_results_ptr =
      kernel_state->memory()->TranslateVirtual<SEARCH_RESULTS*>(
          search_data->search_results_ptr);

  xam::XUSER_CONTEXT* search_contexts_ptr =
      kernel_state->memory()->TranslateVirtual<xam::XUSER_CONTEXT*>(
          search_data->ctx_ptr);

  xam::XUSER_PROPERTY* search_properties_ptr =
      kernel_state->memory()->TranslateVirtual<xam::XUSER_PROPERTY*>(
          search_data->props_ptr);

  util::XLastMatchmakingQuery* matchmaking_query = nullptr;

  if (kernel_state->emulator()->game_info_database()->HasXLast()) {
    matchmaking_query = kernel_state->emulator()
                            ->game_info_database()
                            ->GetXLast()
                            ->GetMatchmakingQuery();

    const auto paramaters =
        matchmaking_query->GetParameters(search_data->proc_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(search_data->proc_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(search_data->proc_index);
    const auto returns = matchmaking_query->GetReturns(search_data->proc_index);

    XELOGI("Matchmaking Query Name: {}",
           matchmaking_query->GetName(search_data->proc_index));

    for (uint32_t i = 0; i < search_data->num_ctx; i++) {
      xam::XUSER_CONTEXT& context = search_contexts_ptr[i];

      auto user =
          kernel_state->xam_state()->GetUserProfile(search_data->user_index);

      std::u16string context_desc =
          kernel_state->xam_state()->user_tracker()->GetContextDescription(
              user->xuid(), context.context_id);

      XELOGD(xe::to_utf8(context_desc));
    }

    for (uint32_t i = 0; i < search_data->num_props; i++) {
      xam::XUSER_PROPERTY& property = search_properties_ptr[i];

      std::u16string property_desc =
          kernel_state->xam_state()->user_tracker()->GetPropertyDescription(
              property.property_id);

      XELOGD(xe::to_utf8(property_desc));
    }
  }

  for (uint32_t i = 0; i < session_count; i++) {
    std::vector<xam::Property> contexts = {};
    std::vector<xam::Property> properties = {};

    const auto all_properties =
        XLiveAPI::SessionPropertiesGet(sessions.at(i)->SessionID_UInt());

    for (const auto& property : all_properties) {
      if (property.IsContext()) {
        contexts.push_back(property);
      } else {
        properties.push_back(property);
      }
    }

    FillSessionContext(kernel_state->memory(), search_data->proc_index,
                       matchmaking_query, contexts, search_data->num_ctx,
                       search_contexts_ptr,
                       &search_results_ptr->results_ptr[i]);
    FillSessionProperties(kernel_state->memory(), search_data->proc_index,
                          matchmaking_query, properties, search_data->num_props,
                          search_properties_ptr,
                          &search_results_ptr->results_ptr[i]);
  }

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetWeightedSessions(
    KernelState* kernel_state,
    XGI_SESSION_SEARCH_WEIGHTED* weighted_search_data, uint32_t num_users) {
  XGI_SESSION_SEARCH search_data = {};

  search_data.proc_index = weighted_search_data->proc_index;
  search_data.user_index = weighted_search_data->user_index;
  search_data.num_results = weighted_search_data->num_results;
  search_data.num_props = weighted_search_data->num_props;
  search_data.num_ctx = weighted_search_data->num_ctx;
  search_data.props_ptr =
      weighted_search_data->non_weighted_search_properties_ptr;
  search_data.ctx_ptr = weighted_search_data->non_weighted_search_contexts_ptr;
  search_data.results_buffer_size = weighted_search_data->results_buffer_size;
  search_data.search_results_ptr = weighted_search_data->search_results_ptr;

  // TODO:
  // weighted_search_data->weighted_search_contexts_ptr;
  // weighted_search_data->weighted_search_properties_ptr;
  // weighted_search_data->num_weighted_properties;
  // weighted_search_data->num_weighted_contexts;

  return GetSessions(kernel_state, &search_data, num_users);
}

X_RESULT XSession::GetSessionByID(Memory* memory,
                                  XGI_SESSION_SEARCH_BYID* search_data) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size = sizeof(XSESSION_SEARCHRESULT);
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  if (search_data->user_index < 0 ||
      search_data->user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  const uint32_t session_count = 1;

  GetSessionByIDs(memory, &search_data->session_id, session_count,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionByIDs(Memory* memory,
                                   XGI_SESSION_SEARCH_BYIDS* search_data) {
  if (!search_data->results_buffer_size) {
    search_data->results_buffer_size =
        search_data->num_session_ids * sizeof(XSESSION_SEARCHRESULT);
    return X_ONLINE_E_SESSION_INSUFFICIENT_BUFFER;
  }

  if (search_data->user_index < 0 ||
      search_data->user_index >= XUserMaxUserCount) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (search_data->num_session_ids <= 0 &&
      search_data->num_session_ids > 0x64) {
    return X_ERROR_INVALID_PARAMETER;
  }

  XNKID* session_ids_ptr =
      memory->TranslateVirtual<XNKID*>(search_data->session_ids_ptr);

  GetSessionByIDs(memory, session_ids_ptr, search_data->num_session_ids,
                  search_data->search_results_ptr,
                  search_data->results_buffer_size);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionByIDs(Memory* memory, XNKID* session_ids_ptr,
                                   uint32_t num_session_ids,
                                   uint32_t search_results_ptr,
                                   uint32_t results_buffer_size) {
  SEARCH_RESULTS* search_results =
      memory->TranslateVirtual<SEARCH_RESULTS*>(search_results_ptr);

  const uint32_t session_search_result_ptr =
      memory->SystemHeapAlloc(results_buffer_size);

  search_results->results_ptr =
      memory->TranslateVirtual<XSESSION_SEARCHRESULT*>(
          session_search_result_ptr);

  uint32_t result_index = 0;

  for (uint32_t i = 0; i < num_session_ids; i++) {
    const xe::be<uint64_t> session_id = XNKIDtoUint64(&session_ids_ptr[i]);

    if (!IsValidXNKID(session_id)) {
      continue;
    }

    const auto session = XLiveAPI::XSessionGet(session_id);

    if (!session->HostAddress().empty()) {
      // HUH? How it should be filled in this case?
      FillSessionContext(memory, 0, nullptr, {}, 0, nullptr,
                         &search_results->results_ptr[result_index]);
      FillSessionProperties(memory, 0, nullptr, {}, 0, nullptr,
                            &search_results->results_ptr[result_index]);
      FillSessionSearchResult(session,
                              &search_results->results_ptr[result_index]);

      result_index++;
    }
  }

  search_results->header.search_results_count = result_index;
  search_results->header.search_results_ptr = session_search_result_ptr;

  return X_ERROR_SUCCESS;
}

void XSession::GetXnAddrFromSessionObject(SessionObjectJSON* session,
                                          XNADDR* XnAddr_ptr) {
  memset(XnAddr_ptr, 0, sizeof(XNADDR));

  // We only store online IP on server.

  // if (XLiveAPI::IsOnline()) {
  // } else {
  // }

  XnAddr_ptr->inaOnline = ip_to_in_addr(session->HostAddress());
  XnAddr_ptr->ina = ip_to_in_addr(session->HostAddress());

  const MacAddress mac = MacAddress(session->MacAddress());

  memcpy(&XnAddr_ptr->abEnet, mac.raw(), sizeof(MacAddress));

  XnAddr_ptr->wPortOnline = session->Port();
}

void XSession::FillSessionSearchResult(
    const std::unique_ptr<SessionObjectJSON>& session,
    XSESSION_SEARCHRESULT* result) {
  result->filled_private_slots = session->FilledPrivateSlotsCount();
  result->filled_public_slots = session->FilledPublicSlotsCount();
  result->open_private_slots = session->OpenPrivateSlotsCount();
  result->open_public_slots = session->OpenPublicSlotsCount();

  Uint64toXNKID(session->SessionID_UInt(), &result->info.sessionID);

  GetXnAddrFromSessionObject(session.get(), &result->info.hostAddress);

  GenerateIdentityExchangeKey(&result->info.keyExchangeKey);
}

void XSession::FillSessionContext(
    Memory* memory, uint32_t matchmaking_index,
    util::XLastMatchmakingQuery* matchmaking_query,
    std::vector<xam::Property> contexts, uint32_t filter_contexts_count,
    xam::XUSER_CONTEXT* filter_contexts_ptr, XSESSION_SEARCHRESULT* result) {
  if (matchmaking_query) {
    const auto paramaters = matchmaking_query->GetParameters(matchmaking_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(matchmaking_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(matchmaking_index);
    const auto returns = matchmaking_query->GetReturns(matchmaking_index);
  }

  result->contexts_count = static_cast<uint32_t>(contexts.size());

  const uint32_t context_ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(
      sizeof(xam::XUSER_CONTEXT) * result->contexts_count));

  xam::XUSER_CONTEXT* contexts_to_get =
      memory->TranslateVirtual<xam::XUSER_CONTEXT*>(context_ptr);

  for (uint32_t i = 0; i < filter_contexts_count; i++) {
    xam::XUSER_CONTEXT& filter_context = filter_contexts_ptr[i];
  }

  uint32_t i = 0;
  for (const auto& context : contexts) {
    contexts_to_get[i].context_id = context.GetPropertyId().value;
    contexts_to_get[i].value = context.get_data()->data.u32;
    i++;
  }

  result->contexts_ptr = context_ptr;
}

void XSession::FillSessionProperties(
    Memory* memory, uint32_t matchmaking_index,
    util::XLastMatchmakingQuery* matchmaking_query,
    std::vector<xam::Property> properties, uint32_t filter_properties_count,
    xam::XUSER_PROPERTY* filter_properties_ptr, XSESSION_SEARCHRESULT* result) {
  if (matchmaking_query) {
    const auto paramaters = matchmaking_query->GetParameters(matchmaking_index);
    const auto filters_left =
        matchmaking_query->GetFiltersLeft(matchmaking_index);
    const auto filters_right =
        matchmaking_query->GetFiltersRight(matchmaking_index);
    const auto returns = matchmaking_query->GetReturns(matchmaking_index);
  }

  result->properties_count = static_cast<uint32_t>(properties.size());

  const uint32_t properties_ptr = memory->SystemHeapAlloc(static_cast<uint32_t>(
      sizeof(xam::XUSER_PROPERTY) * result->properties_count));

  xam::XUSER_PROPERTY* properties_to_set =
      memory->TranslateVirtual<xam::XUSER_PROPERTY*>(properties_ptr);

  for (uint32_t i = 0; i < filter_properties_count; i++) {
    xam::XUSER_PROPERTY& filter_property = filter_properties_ptr[i];
  }

  uint32_t i = 0;
  for (const auto& property : properties) {
    if (property.requires_additional_data()) {
      properties_to_set[i].data.data.unicode.ptr = memory->SystemHeapAlloc(
          static_cast<uint32_t>(property.get_data()->data.unicode.size));
    }

    property.WriteToGuest(&properties_to_set[i]);
    i++;
  }

  result->properties_ptr = properties_ptr;
}

void XSession::PrintSessionDetails() {
  XELOGI(
      "\n***************** PrintSessionDetails *****************\n"
      "UserIndex: {}\n"
      "GameType: {}\n"
      "GameMode: {}\n"
      "eState: {}\n"
      "Nonce: {:016X}\n"
      "Flags: {:08X}\n"
      "MaxPrivateSlots: {}\n"
      "MaxPublicSlots: {}\n"
      "AvailablePrivateSlots: {}\n"
      "AvailablePublicSlots: {}\n"
      "ActualMemberCount: {}\n"
      "ReturnedMemberCount: {}\n"
      "xnkidArbitration: {:016X}\n",
      local_details_.UserIndexHost.get(),
      local_details_.GameType ? "Standard" : "Ranked",
      local_details_.GameMode.get(),
      static_cast<uint32_t>(local_details_.eState), local_details_.Nonce.get(),
      local_details_.Flags.get(), local_details_.MaxPrivateSlots.get(),
      local_details_.MaxPublicSlots.get(),
      local_details_.AvailablePrivateSlots.get(),
      local_details_.AvailablePublicSlots.get(),
      local_details_.ActualMemberCount.get(),
      local_details_.ReturnedMemberCount.get(),
      local_details_.xnkidArbitration.as_uintBE64());

  uint32_t index = 0;

  for (const auto& [xuid, mamber] : local_members_) {
    XELOGI(
        "\n***************** LOCAL MEMBER {} *****************\n"
        "Online XUID: {:016X}\n"
        "UserIndex: {}\n"
        "Flags: {:08X}\n"
        "IsPrivate: {}\n",
        index++, mamber.OnlineXUID.get(), mamber.UserIndex.get(),
        mamber.Flags.get(), mamber.IsPrivate() ? "True" : "False");
  }

  index = 0;

  for (const auto& [xuid, mamber] : remote_members_) {
    XELOGI(
        "\n***************** REMOTE MEMBER {} *****************\n"
        "Online XUID: {:016X}\n"
        "UserIndex: {}\n"
        "Flags: {:08X}\n"
        "IsPrivate: {}\n",
        index++, mamber.OnlineXUID.get(), mamber.UserIndex.get(),
        mamber.Flags.get(), mamber.IsPrivate() ? "True" : "False");
  }
}

void XSession::PrintSessionType(SessionFlags flags) {
  std::string session_description = "";

  if (!flags) {
    XELOGI("Session Flags Empty!");
    return;
  }

  const std::map<SessionFlags, std::string> basic = {
      {HOST, "Host"},
      {PRESENCE, "Presence"},
      {STATS, "Stats"},
      {MATCHMAKING, "Matchmaking"},
      {ARBITRATION, "Arbitration"},
      {PEER_NETWORK, "Peer Network"},
      {SOCIAL_MATCHMAKING_ALLOWED, "Social Matchmaking"},
      {INVITES_DISABLED, "No invites"},
      {JOIN_VIA_PRESENCE_DISABLED, "Presence Join Disabled"},
      {JOIN_IN_PROGRESS_DISABLED, "In-Progress Join Disabled"},
      {JOIN_VIA_PRESENCE_FRIENDS_ONLY, "Friends Only"}};

  const std::map<SessionFlags, std::string> extended = {
      {SINGLEPLAYER_WITH_STATS, "Singleplayer with Stats"},
      {LIVE_MULTIPLAYER_STANDARD, "LIVE: Multiplayer"},
      {LIVE_MULTIPLAYER_RANKED, "LIVE: Multiplayer Ranked"},
      {GROUP_LOBBY, "Group Lobby"},
      {GROUP_GAME, "Group Game"}};

  for (const auto& entry : basic) {
    if (HasSessionFlag(flags, entry.first)) {
      session_description.append(entry.second + ", ");
    }
  }

  XELOGI("Session Description: {}", session_description);
  session_description.clear();

  for (const auto& entry : extended) {
    if (HasSessionFlag(flags, entry.first)) {
      session_description.append(entry.second + ", ");
    }
  }

  XELOGI("Session Extended Description: {}", session_description);
}

}  // namespace kernel
}  // namespace xe