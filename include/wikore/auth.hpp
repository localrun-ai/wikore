#pragma once
#include <string>
#include <optional>
#include <drogon/HttpFilter.h>
#include "wikore/config.hpp"

namespace wikore {

// Load OIDC JWKS from cfg.oidc_issuer and seed the in-memory key cache.
// Call once in main() before drogon::app().run().
// No-op (warns) if cfg.oidc_issuer is empty.
void auth_init(const Config& cfg);

// Claims extracted from a validated JWT or API key.
struct Identity {
    std::string user_id;       // UUID string (SSO sub claim)
    std::string email;
    std::string display_name;
    bool        is_admin = false;
};

// Validates a JWT (RS256) against the configured OIDC issuer JWKS.
// Returns the identity on success, nullopt if invalid/expired. Used by
// AuthFilter; the internal user (users.id) is resolved asynchronously there.
std::optional<Identity> validate_jwt(std::string_view token);

// Drogon filter: sets req->getAttributes()->insert("identity", ...) on success,
// returns 401 on failure. Applied to all /api/* routes except /api/health.
class AuthFilter : public drogon::HttpFilter<AuthFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&&       stop,
                  drogon::FilterChainCallback&&  next) override;
};

// Drogon filter: requires identity.is_admin == true.
// Must be chained after AuthFilter.
class AdminFilter : public drogon::HttpFilter<AdminFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&&       stop,
                  drogon::FilterChainCallback&&  next) override;
};

} // namespace wikore
