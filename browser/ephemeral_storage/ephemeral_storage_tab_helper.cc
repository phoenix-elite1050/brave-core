/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/ephemeral_storage/ephemeral_storage_tab_helper.h"

#include <map>
#include <set>

#include "base/feature_list.h"
#include "base/hash/md5.h"
#include "base/no_destructor.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/session_storage_namespace.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/base/features.h"
#include "net/base/url_util.h"

using content::BrowserContext;
using content::NavigationHandle;
using content::SessionStorageNamespace;
using content::WebContents;

namespace ephemeral_storage {

namespace {

// Session storage ids are expected to be 36 character long GUID strings. Since
// we are constructing our own ids, we convert our string into a 32 character
// hash and then use that make up our own GUID-like string. Because of the way
// we are constructing the string we should never collide with a real GUID and
// we only need to worry about hash collisions, which are unlikely.
std::string StringToSessionStorageId(const std::string& string,
                                     const std::string& suffix) {
  std::string hash = base::MD5String(string + suffix) + "____";
  DCHECK_EQ(hash.size(), 36u);
  return hash;
}

}  // namespace

// EphemeralStorageTabHelper helps to manage the lifetime of ephemeral storage.
// For more information about the design of ephemeral storage please see the
// design document at:
// https://github.com/brave/brave-browser/wiki/Ephemeral-Storage-Design
EphemeralStorageTabHelper::EphemeralStorageTabHelper(WebContents* web_contents)
    : WebContentsObserver(web_contents) {
  DCHECK(base::FeatureList::IsEnabled(net::features::kBraveEphemeralStorage));

  // The URL might not be empty if this is a restored WebContents, for instance.
  // In that case we want to make sure it has valid ephemeral storage.
  const GURL& url = web_contents->GetLastCommittedURL();
  if (!url.is_empty())
    CreateEphemeralStorageAreasForDomainAndURL(
        net::URLToEphemeralStorageDomain(url), url);
}

EphemeralStorageTabHelper::~EphemeralStorageTabHelper() {}

void EphemeralStorageTabHelper::ReadyToCommitNavigation(
    NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInMainFrame())
    return;
  if (navigation_handle->IsSameDocument())
    return;

  const GURL& new_url = navigation_handle->GetURL();
  std::string new_domain = net::URLToEphemeralStorageDomain(new_url);
  std::string previous_domain =
      net::URLToEphemeralStorageDomain(web_contents()->GetLastCommittedURL());
  if (new_domain == previous_domain)
    return;

  CreateEphemeralStorageAreasForDomainAndURL(new_domain, new_url);
}

void EphemeralStorageTabHelper::CreateEphemeralStorageAreasForDomainAndURL(
    std::string new_domain,
    const GURL& new_url) {
  auto* browser_context = web_contents()->GetBrowserContext();
  auto site_instance =
      content::SiteInstance::CreateForURL(browser_context, new_url);
  auto* partition =
      BrowserContext::GetStoragePartition(browser_context, site_instance.get());

  // This will fetch a session storage namespace for this storage partition
  // and storage domain. If another tab helper is already using the same
  // namespace, this will just give us a new reference. When the last tab helper
  // drops the reference, the namespace should be deleted.
  std::string local_partition_id =
      StringToSessionStorageId(new_domain, "/ephemeral-local-storage");
  local_storage_namespace_ =
      content::CreateSessionStorageNamespace(partition, local_partition_id);

  // Session storage is always per-tab and never per-TLD, so we always delete
  // and recreate the session storage when switching domains.
  //
  // We need to explicitly release the storage namespace before recreating a
  // new one in order to make sure that we remove the final reference and free
  // it.
  session_storage_namespace_.reset();

  std::string session_partition_id = StringToSessionStorageId(
      content::GetSessionStorageNamespaceId(web_contents()),
      "/ephemeral-session-storage");
  session_storage_namespace_ =
      content::CreateSessionStorageNamespace(partition, session_partition_id);

  tld_ephemeral_lifetime_ = content::TLDEphemeralLifetime::GetOrCreate(
      browser_context, partition, new_domain);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(EphemeralStorageTabHelper)

}  // namespace ephemeral_storage
