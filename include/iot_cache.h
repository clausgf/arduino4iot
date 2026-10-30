/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "iot_ports.h"
#include "iot_result.h"

// *****************************************************************************
// Conditional GET: fetch a resource only if it changed since the last fetch,
// @see IotApi::getCached().
//
// The cache validators (ETag, Last-Modified) are passed in and handed back,
// never stored here. The caller decides when the new ones count - typically
// only after the body was used successfully, so a resource that failed to
// apply is fetched again next cycle instead of answered with 304. Where they
// live (NVS, RTC RAM, nowhere) is the caller's choice; IotStoredValidators
// covers the IotStore case.
// *****************************************************************************

/// The validators of a cached copy. Empty means "no copy", which makes the
/// request unconditional.
struct IotCacheValidators
{
    std::string etag;
    std::string lastModified;

    bool isEmpty() const { return etag.empty() && lastModified.empty(); }
    bool operator==(const IotCacheValidators& other) const
    {
        return etag == other.etag && lastModified == other.lastModified;
    }
    bool operator!=(const IotCacheValidators& other) const { return !(*this == other); }
};

struct IotCachedResponse
{
    /// Ok (2xx) with a new body, isNotModified() (304) if the cached copy is
    /// current, otherwise the HTTP or transport error.
    IotResult result;
    /// The resource; empty unless result is Ok.
    std::string body;
    /// The validators to commit once the body was used: from the response on a
    /// 2xx, the ones passed in (updated by any the server resent) on a 304, the
    /// ones passed in unchanged on an error.
    IotCacheValidators validators;
    /// Cache-Control max-age in seconds, -1 if absent or if the response must
    /// not be reused without revalidation (no-cache, no-store). nice4iot's own
    /// file endpoint sends no-cache; a forwarded upstream may send max-age.
    int32_t maxAge_s = -1;
};

/**
 * @return the max-age of a Cache-Control header value in seconds, -1 if it has
 *         none, an invalid one, or no-cache / no-store.
 */
int32_t iotParseMaxAge(const std::string& cacheControl);

/**
 * Cache validators kept in an IotStore under two keys, for resources whose
 * cached copy survives a power loss.
 *
 *   static IotStoredValidators imageCache(api.store(), "imgEtag", "imgDate");
 *   IotCachedResponse r = api.getCached(path, imageCache.load());
 *   if (r.result.isOk() && use(r.body)) { imageCache.commit(r.validators); }
 */
class IotStoredValidators
{
public:
    /// The keys must fit IotStore::MAX_KEY_LENGTH; @p store must outlive this.
    IotStoredValidators(IotStore& store, const char* etagKey, const char* lastModifiedKey):
        _store(store), _etagKey(etagKey), _lastModifiedKey(lastModifiedKey) {}

    IotCacheValidators load() const;

    /// Persist @p validators. Writes nothing when unchanged.
    void commit(const IotCacheValidators& validators);

    /// Forget the cached copy, so the next request is unconditional.
    void clear() { commit(IotCacheValidators{}); }

private:
    IotStore& _store;
    const char* _etagKey;
    const char* _lastModifiedKey;
};
