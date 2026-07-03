// JWT RS256 authentication + API key validation.
//
// Auth flow (per request):
//   1. Bearer token  -> validate_jwt()  -> synchronous, fast path
//   2. X-API-Key     -> validate_api_key_async() -> Redis cache, then DB async
//
// JWKS:
//   - Fetched synchronously at startup via OpenSSL BIO (no extra deps).
//   - Cached in-memory; a background thread refreshes when a kid is missing
//     and the cache is older than 5 minutes (handles key rotation).
//   - One RSA key per kid stored as PEM string; jwt-cpp verifies RS256.
//
// API key:
//   - SHA-256(raw_key) looked up in Redis (lr:api_key:{hash}, 5 min TTL).
//   - On miss: async DB query -> cache result.
//
// Identity is serialized as JSON for Redis storage via glaze auto-reflection.

#include "wikore/auth.hpp"
#include "wikore/config.hpp"
#include "wikore/redis.hpp"
#include "wikore/db.hpp"

#include <jwt-cpp/jwt.h>
#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>
#include <drogon/drogon.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wikore {

// ---------------------------------------------------------------------------
// glaze meta: must be in the glz namespace, defined after the type
// ---------------------------------------------------------------------------

} // namespace wikore

template<>
struct glz::meta<wikore::Identity> {
    using T = wikore::Identity;
    static constexpr auto value = glz::object(
        "user_id",      &T::user_id,
        "email",        &T::email,
        "display_name", &T::display_name,
        "is_admin",     &T::is_admin
    );
};

namespace wikore {

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

namespace {

struct JwkEntry {
    std::string kty{};
    std::string kid{};
    std::string n{};
    std::string e{};
    std::string use{};
};

struct JwksDoc {
    std::vector<JwkEntry> keys{};
};

struct JwksCache {
    std::shared_mutex mu;
    std::map<std::string, std::string> kid_to_pem; // kid -> RSA public key PEM
    std::chrono::steady_clock::time_point fetched_at{};
    bool loaded = false;
};

static JwksCache        g_jwks;
static const Config*    g_cfg = nullptr;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static std::vector<uint8_t> b64url_decode(std::string_view input) {
    std::string s(input);
    while (s.size() % 4 != 0) s += '=';
    for (char& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    std::vector<uint8_t> out(s.size() * 3 / 4 + 4);
    int n = EVP_DecodeBlock(out.data(),
                            reinterpret_cast<const unsigned char*>(s.c_str()),
                            static_cast<int>(s.size()));
    if (n < 0) throw std::runtime_error("base64url decode error");
    int pad = static_cast<int>(s.ends_with("==") ? 2 : s.ends_with("=") ? 1 : 0);
    out.resize(static_cast<size_t>(n) - static_cast<size_t>(pad));
    return out;
}

static std::string sha256_hex(std::string_view data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (unsigned char b : hash)
        hex += std::format("{:02x}", b);
    return hex;
}

// Construct an RSA public-key PEM from the JWK base64url-encoded modulus and
// exponent. Uses OpenSSL 3.x EVP_PKEY_fromdata to avoid the deprecated RSA_*
// low-level API.
static std::string jwk_to_pem(std::string_view n_b64, std::string_view e_b64) {
    auto n_bytes = b64url_decode(n_b64);
    auto e_bytes = b64url_decode(e_b64);

    // OSSL_PARAM_construct_BN reads the buffer as an unsigned big-endian
    // integer, which is exactly the JWK wire format.
    OSSL_PARAM params[3];
    params[0] = OSSL_PARAM_construct_BN(
        OSSL_PKEY_PARAM_RSA_N,
        reinterpret_cast<unsigned char*>(n_bytes.data()), n_bytes.size());
    params[1] = OSSL_PARAM_construct_BN(
        OSSL_PKEY_PARAM_RSA_E,
        reinterpret_cast<unsigned char*>(e_bytes.data()), e_bytes.size());
    params[2] = OSSL_PARAM_construct_end();

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
    if (!ctx) throw std::runtime_error("EVP_PKEY_CTX_new_from_name(RSA) failed");

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0 || !pkey) {
        EVP_PKEY_CTX_free(ctx);
        throw std::runtime_error("EVP_PKEY_fromdata(RSA) failed: " +
                                 std::string(ERR_reason_error_string(ERR_get_error())));
    }
    EVP_PKEY_CTX_free(ctx);

    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, pkey);
    EVP_PKEY_free(pkey);

    char* data = nullptr;
    long  len  = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(len));
    BIO_free(bio);
    return pem;
}

// ---------------------------------------------------------------------------
// Synchronous HTTP(S) GET for JWKS fetch at startup.
//
// Using OpenSSL BIO rather than Drogon's async HttpClient because at startup
// the Drogon event loop is not yet running. This is called at most once per
// process lifetime (plus rare background refreshes on key rotation).
// ---------------------------------------------------------------------------

struct ParsedUrl {
    std::string scheme, host, path;
    int port = 80;
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl p;
    auto sep = url.find("://");
    if (sep == std::string::npos) throw std::runtime_error("bad URL: " + url);
    p.scheme = url.substr(0, sep);
    p.port   = (p.scheme == "https") ? 443 : 80;

    size_t rest  = sep + 3;
    size_t slash = url.find('/', rest);
    std::string hostport = (slash == std::string::npos)
        ? url.substr(rest)
        : url.substr(rest, slash - rest);
    p.path = (slash == std::string::npos) ? "/" : url.substr(slash);

    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        p.host = hostport.substr(0, colon);
        p.port = std::stoi(hostport.substr(colon + 1));
    } else {
        p.host = hostport;
    }
    return p;
}

// Extract the body from a raw HTTP/1.x response string.
static std::string strip_http_headers(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) throw std::runtime_error("malformed HTTP response");
    return resp.substr(pos + 4);
}

static std::string https_get_body(const ParsedUrl& u) {
    std::string addr = u.host + ":" + std::to_string(u.port);

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

    BIO* bio = BIO_new_ssl_connect(ctx);
    BIO_set_conn_hostname(bio, addr.c_str());

    SSL* ssl = nullptr;
    BIO_get_ssl(bio, &ssl);
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
    SSL_set_tlsext_host_name(ssl, u.host.c_str());

    if (BIO_do_connect(bio) <= 0 || BIO_do_handshake(bio) <= 0) {
        BIO_free_all(bio); SSL_CTX_free(ctx);
        throw std::runtime_error("TLS connect failed: " + u.host);
    }

    std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host +
                      "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    BIO_write(bio, req.c_str(), static_cast<int>(req.size()));

    std::string resp;
    char buf[4096];
    int n;
    while ((n = BIO_read(bio, buf, static_cast<int>(sizeof(buf)))) > 0)
        resp.append(buf, static_cast<size_t>(n));

    BIO_free_all(bio);
    SSL_CTX_free(ctx);
    return strip_http_headers(resp);
}

static std::string http_get_body(const ParsedUrl& u) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(u.port);
    if (getaddrinfo(u.host.c_str(), port_str.c_str(), &hints, &res) != 0)
        throw std::runtime_error("getaddrinfo failed: " + u.host);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res); close(fd);
        throw std::runtime_error("connect failed: " + u.host);
    }
    freeaddrinfo(res);

    std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host +
                      "\r\nAccept: application/json\r\nConnection: close\r\n\r\n";
    ::send(fd, req.c_str(), req.size(), 0);

    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<size_t>(n));
    close(fd);
    return strip_http_headers(resp);
}

static std::string fetch_url(const std::string& url) {
    auto u = parse_url(url);
    return (u.scheme == "https") ? https_get_body(u) : http_get_body(u);
}

// ---------------------------------------------------------------------------
// JWKS refresh
// ---------------------------------------------------------------------------

static void do_refresh_jwks(const std::string& oidc_issuer) {
    std::string jwks_url = oidc_issuer;
    if (!jwks_url.empty() && jwks_url.back() == '/') jwks_url.pop_back();
    jwks_url += "/.well-known/jwks.json";

    std::string body;
    try { body = fetch_url(jwks_url); }
    catch (const std::exception& ex) {
        spdlog::error("[auth] JWKS fetch failed ({}): {}", jwks_url, ex.what());
        return;
    }

    JwksDoc doc;
    auto err = glz::read<glz::opts{.error_on_unknown_keys = false}>(doc, body);
    if (err) {
        spdlog::error("[auth] JWKS parse error: {}", glz::format_error(err, body));
        return;
    }

    std::map<std::string, std::string> new_keys;
    for (const auto& k : doc.keys) {
        if (k.kty != "RSA" || k.n.empty() || k.e.empty()) continue;
        try {
            new_keys[k.kid] = jwk_to_pem(k.n, k.e);
        } catch (const std::exception& ex) {
            spdlog::warn("[auth] JWKS: skip kid={}: {}", k.kid, ex.what());
        }
    }

    std::unique_lock lk(g_jwks.mu);
    g_jwks.kid_to_pem = std::move(new_keys);
    g_jwks.fetched_at = std::chrono::steady_clock::now();
    g_jwks.loaded     = true;
    spdlog::info("[auth] JWKS loaded: {} key(s)", g_jwks.kid_to_pem.size());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public: auth_init
// ---------------------------------------------------------------------------

void auth_init(const Config& cfg) {
    g_cfg = &cfg;
    if (cfg.oidc_issuer.empty()) {
        spdlog::warn("[auth] OIDC_ISSUER not set - JWT auth disabled");
        return;
    }
    do_refresh_jwks(cfg.oidc_issuer);
}

// ---------------------------------------------------------------------------
// JWT validation
// ---------------------------------------------------------------------------

std::optional<Identity> validate_jwt(std::string_view token) {
    if (!g_cfg || g_cfg->oidc_issuer.empty() || token.empty())
        return std::nullopt;
    try {
        auto decoded = jwt::decode(std::string(token));

        std::string kid;
        if (decoded.has_key_id()) kid = decoded.get_key_id();

        std::string pem;
        {
            std::shared_lock lk(g_jwks.mu);
            auto it = g_jwks.kid_to_pem.find(kid);
            if (it == g_jwks.kid_to_pem.end()) {
                // kid not cached; trigger background refresh if stale
                auto age = std::chrono::steady_clock::now() - g_jwks.fetched_at;
                if (age > std::chrono::minutes{5}) {
                    std::thread([] {
                        if (g_cfg) do_refresh_jwks(g_cfg->oidc_issuer);
                    }).detach();
                }
                return std::nullopt;
            }
            pem = it->second;
        }

        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::rs256(pem))
            .with_issuer(g_cfg->oidc_issuer)
            .with_audience(g_cfg->oidc_audience);
        verifier.verify(decoded);

        Identity id;
        id.user_id = decoded.get_subject();
        if (decoded.has_payload_claim("email"))
            id.email = decoded.get_payload_claim("email").as_string();
        if (decoded.has_payload_claim("name"))
            id.display_name = decoded.get_payload_claim("name").as_string();
        else if (decoded.has_payload_claim("preferred_username"))
            id.display_name = decoded.get_payload_claim("preferred_username").as_string();

        // Keycloak puts roles in realm_access.roles[]. Serialize to string and
        // search for "admin" / "wikore-admin" - avoids a full picojson traversal.
        if (decoded.has_payload_claim("realm_access")) {
            auto ra_str = decoded.get_payload_claim("realm_access").to_json().serialize();
            id.is_admin = ra_str.find("\"admin\"")       != std::string::npos ||
                          ra_str.find("\"wikore-admin\"") != std::string::npos;
        }

        // JTI revocation: presence of the key means revoked.
        if (decoded.has_payload_claim("jti")) {
            std::string jti = decoded.get_payload_claim("jti").as_string();
            if (Redis::get("lr:jwt:" + jti).has_value()) return std::nullopt;
        }

        return id;

    } catch (const std::exception& ex) {
        spdlog::debug("[auth] JWT rejected: {}", ex.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// API key validation
// ---------------------------------------------------------------------------

namespace {

// Async variant used by AuthFilter. Falls back to DB on Redis miss.
static drogon::HttpResponsePtr make_401(const char* msg = "authentication required") {
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setStatusCode(drogon::k401Unauthorized);
    r->setBody(std::format(R"({{"error":"{}"}})", msg));
    r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return r;
}

static void validate_api_key_async(
    std::string_view              key,
    const drogon::HttpRequestPtr& req,
    drogon::FilterCallback&&      stop,
    drogon::FilterChainCallback&& next)
{
    std::string hash = sha256_hex(key);

    // No Redis cache on the request route. A key_hash -> Identity cache carries
    // none of the state that can invalidate a credential (key revoked_at /
    // expires_at, user deactivated_at), and nothing evicts it when those
    // change - so a hit could authenticate a revoked/expired/deactivated
    // credential until the TTL expires. Checking any of that requires the
    // api_keys lookup anyway, which makes the cache pointless for
    // authorization. We always run the authoritative query; it checks revoked,
    // expired, and deactivated in one indexed round-trip.
    auto db = drogon::app().getDbClient();
    if (!db) { stop(make_401("service unavailable")); return; }

    static const std::string sql =
        "SELECT u.id, u.email, u.display_name, k.is_admin "
        "FROM api_keys k "
        "JOIN users u ON k.user_id = u.id "
        "WHERE k.key_hash = $1 "
        "  AND k.revoked_at IS NULL "
        "  AND (k.expires_at IS NULL OR k.expires_at > now()) "
        "  AND u.deactivated_at IS NULL "   // soft-deactivated users lose access
        "LIMIT 1";

    // Capture stop/next as shared_ptr so they survive the async callback.
    auto stop_p = std::make_shared<drogon::FilterCallback>(std::move(stop));
    auto next_p = std::make_shared<drogon::FilterChainCallback>(std::move(next));

    db->execSqlAsync(
        sql,
        [req, next_p, stop_p](const drogon::orm::Result& rows) mutable {
            if (rows.empty()) {
                (*stop_p)(make_401("invalid api key"));
                return;
            }
            Identity id;
            id.user_id      = rows[0]["id"].as<std::string>();
            id.email        = rows[0]["email"].as<std::string>();
            id.display_name = rows[0]["display_name"].as<std::string>();
            id.is_admin     = rows[0]["is_admin"].as<bool>();

            req->getAttributes()->insert("identity", id);
            (*next_p)();
        },
        [stop_p](const drogon::orm::DrogonDbException& ex) mutable {
            spdlog::error("[auth] api_key DB error: {}", ex.base().what());
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k503ServiceUnavailable);
            (*stop_p)(r);
        },
        hash);
}

// Resolve a verified JWT to an internal user row and finish the filter chain.
//
// validate_jwt() sets Identity::user_id to the raw OIDC `sub` claim, which the
// schema stores in users.external_sub - NOT users.id. Downstream code (the
// access resolver, request handlers) keys on users.id, so we must translate
// (issuer, sub) -> users.id here. We also drop deactivated users: per
// domain/types.hpp the auth middleware is the single point that rejects them
// before a RequestContext is built.
//
// (issuer, sub) is unique only within a company (V002), so in a deployment
// where one issuer maps to multiple tenants it could match more than one row.
// We fail closed on that ambiguity (reject) rather than guess a tenant.
static void resolve_jwt_user_async(
    Identity                      jwt_id,
    const drogon::HttpRequestPtr& req,
    drogon::FilterCallback&&      stop,
    drogon::FilterChainCallback&& next)
{
    auto db = drogon::app().getDbClient();
    if (!db) { stop(make_401("service unavailable")); return; }

    static const std::string sql =
        "SELECT id FROM users "
        "WHERE external_issuer = $1 AND external_sub = $2 "
        "  AND deactivated_at IS NULL";

    const std::string issuer = g_cfg ? g_cfg->oidc_issuer : std::string{};
    const std::string sub    = jwt_id.user_id;

    auto stop_p = std::make_shared<drogon::FilterCallback>(std::move(stop));
    auto next_p = std::make_shared<drogon::FilterChainCallback>(std::move(next));
    auto id_p   = std::make_shared<Identity>(std::move(jwt_id));

    db->execSqlAsync(
        sql,
        [req, id_p, stop_p, next_p](const drogon::orm::Result& rows) mutable {
            // 0 rows: not provisioned or deactivated. >1: (issuer, sub)
            // ambiguous across tenants - fail closed.
            if (rows.size() != 1) {
                (*stop_p)(make_401("user not provisioned"));
                return;
            }
            id_p->user_id = rows[0]["id"].as<std::string>();
            req->getAttributes()->insert("identity", *id_p);
            (*next_p)();
        },
        [stop_p](const drogon::orm::DrogonDbException& ex) mutable {
            spdlog::error("[auth] jwt user resolve DB error: {}", ex.base().what());
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k503ServiceUnavailable);
            (*stop_p)(r);
        },
        issuer, sub);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Drogon filters
// ---------------------------------------------------------------------------

void AuthFilter::doFilter(const drogon::HttpRequestPtr& req,
                          drogon::FilterCallback&&      stop,
                          drogon::FilterChainCallback&& next) {
    // JWT: token verification is synchronous (keys in memory, ~10us), but
    // translating the OIDC sub to an internal users.id needs a DB lookup, so
    // resolution finishes asynchronously.
    const auto& auth = req->getHeader("Authorization");
    if (auth.starts_with("Bearer ")) {
        auto id = validate_jwt(std::string_view(auth).substr(7));
        if (id) {
            resolve_jwt_user_async(*id, req, std::move(stop), std::move(next));
            return;
        }
    }

    // API key: may need DB, so async
    const auto& api_key = req->getHeader("X-API-Key");
    if (!api_key.empty()) {
        validate_api_key_async(api_key, req, std::move(stop), std::move(next));
        return;
    }

    stop(make_401());
}

void AdminFilter::doFilter(const drogon::HttpRequestPtr& req,
                           drogon::FilterCallback&&      stop,
                           drogon::FilterChainCallback&& next) {
    try {
        const auto& id = req->getAttributes()->get<Identity>("identity");
        if (!id.is_admin) throw std::exception{};
        next();
    } catch (...) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setStatusCode(drogon::k403Forbidden);
        r->setBody(R"({"error":"admin access required"})");
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        stop(r);
    }
}

} // namespace wikore
