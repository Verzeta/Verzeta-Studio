// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file custom-server-registry.h
 * @brief Owns the runtime set of user-configured custom OpenAI-API-
 *        compatible servers: vLLM, LM Studio, Jan, Llamafile,
 *        TabbyAPI, KoboldCpp, LocalAI, SGLang, Oobabooga, or any
 *        other stack that respects the OpenAI `/v1/chat/completions`
 *        + `/v1/models` contract.
 *
 *        Responsibilities:
 *          - Hydrate at construction from `SettingsService::customServersBlob`
 *            (JSON array under settings key `custom_servers`).  For each
 *            persisted entry, construct an `OpenAICompatProvider`
 *            instance with a dedicated `HttpClient`, apply the saved
 *            identity / URL / capability flags / API key (read through
 *            `SettingsService::apiKey("custom.\<slug\>")`)
 *            and register the provider with `ModelRouter`.
 *          - Expose add / update / remove / list / refresh-models
 *            Q_INVOKABLE methods to QML for the setup-sheet UI.
 *          - On every mutation: persist the catalogue back to settings,
 *            emit `serversChanged()`, and trigger `refreshModels()` on
 *            the affected instance so the model dropdown is populated
 *            before the user opens it.
 *
 *        Persistence shape is a JSON array, one object per server:
 *        ```
 *        [
 *          {
 *            "slug":                "lm-studio-home",
 *            "displayName":         "LM Studio at home",
 *            "baseUrl":             "http://localhost:1234/v1",
 *            "requiresApiKeyFlag":  false,
 *            "supportsStreaming":   true,
 *            "supportsToolCalling": true,
 *            "supportsVision":      false
 *          },
 *          ...
 *        ]
 *        ```
 *        API keys are never embedded in this blob. They are stored
 *        separately through `SettingsService::setApiKey("custom.\<slug\>", key)`.
 *
 *        Ownership:
 *          - `OpenAICompatProvider` instances are owned by `ModelRouter`
 *            (registered via `unique_ptr`).  Registry retains a
 *            `QPointer` for control (setBaseUrl / setSupports* /
 *            refreshModels etc).
 *          - `HttpClient` instances are owned by the registry, one per
 *            provider instance, for clean isolation: a stuck server
 *            cannot poison sibling pools.
 *          - Destructor invariant: before the per-row `HttpClient` is
 *            destroyed, the corresponding provider MUST be unregistered
 *            from `ModelRouter` so the provider's `m_http` reference
 *            does not dangle during its destruction.  The registry's
 *            own destructor enforces this for the whole-app shutdown
 *            path; `remove()` enforces it for the per-row removal path.
 *
 *        Slugs:
 *          - URL-safe identifier matching `[a-z0-9-]+`, length [2, 32].
 *          - Generated from the user-typed display name + a
 *            collision-avoidance numeric suffix.
 *          - Stable post-creation.  Rename changes display name only;
 *            the slug is the routing-key composite key in
 *            `conversations.llm_config.provider` + the per-member
 *            override columns and must NOT shift under a member alias.
 *
 *        Threading: strictly main-thread.  Every public method begins
 *        with `VERZETA_ASSERT_MAIN_THREAD()`.
 *
 *        License: LGPL-3.0-or-later.  The registry is infrastructure
 *        rather than the differentiated moat. Provider adapters and
 *        their orchestration are classified alongside the other
 *        OpenAI-family adapters as LGPL infrastructure.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Network; OpenAICompatProvider (API),
 *               HttpClient (Utility); ModelRouter, SettingsService
 *               (non-owning references).
 */

#pragma once

#include <memory>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <vector>

class HttpClient;
class ModelRouter;
class OpenAICompatProvider;
class SettingsService;

/**
 * @brief Catalogue + lifecycle controller for user-configured custom
 *        OpenAI-API-compatible servers.
 *
 * Constructed once at AppController init time.  Hydrates its catalogue
 * from `SettingsService::customServersBlob`, constructs an
 * `OpenAICompatProvider` per persisted entry, and registers each with
 * `ModelRouter`.  All subsequent mutations (`add` / `update` /
 * `remove`) go through this class so persistence, ModelRouter
 * registration state, and signal fan-out stay coherent.
 */
class CustomServerRegistry : public QObject {
    Q_OBJECT
    /// The configured custom servers, as a bindable property so QML
    /// refreshes automatically when the catalogue changes. Same shape as
    /// `list()`; QML should bind `model: CustomServers.servers` rather than
    /// calling the `list()` method (a method call has no change signal, so
    /// a method binding never re-evaluates on its own).
    Q_PROPERTY(QVariantList servers READ list NOTIFY serversChanged)

  public:
    /**
     * @brief Constructs the registry and hydrates from persistence.
     * @param router    Non-owning reference to the application's
     *                  ModelRouter.  The registry registers /
     *                  unregisters foreground providers through this
     *                  reference; the router owns the provider
     *                  instances.
     * @param settings  Non-owning reference to SettingsService for
     *                  catalogue blob persistence and per-server API
     *                  key storage.
     * @param parent    Optional Qt parent.
     *
     * After construction, the catalogue is fully hydrated and every
     * persisted server is registered with ModelRouter.  Model lists
     * are refreshed asynchronously; see `refreshModels(slug)` for
     * the per-instance refresh dispatch and the response timing.
     */
    CustomServerRegistry(ModelRouter& router, SettingsService& settings, QObject* parent = nullptr);

    /**
     * @brief Destructor. Unregisters every owned provider from
     *        ModelRouter BEFORE the per-row HttpClients are destroyed
     *        so dangling `m_http` references inside dying providers
     *        cannot occur.
     */
    ~CustomServerRegistry() override;

    // -----------------------------------------------------------------------
    // QML reads
    // -----------------------------------------------------------------------

    /**
     * @brief Lists every configured custom server.
     * @returns QVariantList of maps with keys: slug, displayName,
     *          baseUrl, requiresApiKeyFlag, hasApiKey,
     *          supportsStreaming, supportsToolCalling, supportsVision,
     *          modelCount, lastRefreshStatus.  API key values are
     *          NEVER returned through this surface.
     */
    Q_INVOKABLE QVariantList list() const;

    /**
     * @brief Returns one server's metadata by slug.
     * @param slug Slug returned by an earlier `add()` call or read
     *             from `list()`.
     * @returns Same shape as a single entry in `list()`; empty map
     *          when the slug is unknown.
     */
    Q_INVOKABLE QVariantMap byslug(const QString& slug) const;

    /**
     * @brief Returns the slug of an already-configured server whose base
     *        URL matches @p baseUrl (normalised: trimmed, lowercase
     *        scheme+host, trailing slash dropped), or empty if none.
     * @param baseUrl Candidate base URL.
     * @returns Existing slug for that URL, or empty QString.
     *
     * Lets the setup sheet warn the user that a provider for this URL is
     * already configured instead of silently creating a duplicate.
     */
    Q_INVOKABLE QString slugForBaseUrl(const QString& baseUrl) const;

    // -----------------------------------------------------------------------
    // QML writes
    // -----------------------------------------------------------------------

    /**
     * @brief Adds a new custom server, registers it with ModelRouter,
     *        persists the catalogue, and triggers an asynchronous
     *        model-list refresh.
     * @param displayName         User-facing name; used as the slug
     *                            seed via `generateSlug`.
     * @param baseUrl             Base URL pointing at the server's
     *                            OpenAI-compatible root, conventionally
     *                            ending in `/v1` (e.g.
     *                            `http://localhost:1234/v1`).
     * @param requiresApiKeyFlag  When true, the empty-key fail-fast
     *                            gate fires even when no key is set.
     * @param apiKey              API key value, or empty for keyless
     *                            servers.  Stored through SettingsService
     *                            under the routing key
     *                            `custom.\<slug\>`.
     * @param supportsStreaming   User-declared streaming-support flag.
     * @param supportsToolCalling User-declared tool-calling flag.
     * @param supportsVision      User-declared vision-input flag.
     * @returns Generated slug on success; empty string on failure
     *          (invalid base URL, persistence failure).
     */
    Q_INVOKABLE QString add(const QString& displayName,
                            const QString& baseUrl,
                            bool requiresApiKeyFlag,
                            const QString& apiKey,
                            bool supportsStreaming,
                            bool supportsToolCalling,
                            bool supportsVision);

    /**
     * @brief Updates an existing custom server in place.
     * @param slug                Existing slug; must match a row.
     * @param displayName         New display name.
     * @param baseUrl             New base URL.
     * @param requiresApiKeyFlag  New flag value.
     * @param apiKey              New API key value (pass empty to
     *                            clear).
     * @param supportsStreaming   New streaming flag.
     * @param supportsToolCalling New tool-calling flag.
     * @param supportsVision      New vision flag.
     * @returns true on success; false when the slug is unknown.
     *
     * When base URL or API key changes, the model list is
     * re-refreshed asynchronously so the dropdown reflects the new
     * server.  The provider instance is updated in place; its slug
     * (and therefore its routing key) does not change.
     */
    Q_INVOKABLE bool update(const QString& slug,
                            const QString& displayName,
                            const QString& baseUrl,
                            bool requiresApiKeyFlag,
                            const QString& apiKey,
                            bool supportsStreaming,
                            bool supportsToolCalling,
                            bool supportsVision);

    /**
     * @brief Removes a configured custom server.
     * @param slug Existing slug.
     * @returns true on success; false when the slug is unknown.
     *
     * Order of operations:
     *   1. Unregister the provider from ModelRouter. The unique_ptr
     *      destructor in ModelRouter destroys the provider instance.
     *   2. Clear the stored API key.
     *   3. Destroy the per-row HttpClient.
     *   4. Erase the row.
     *   5. Persist the new catalogue.
     *   6. Emit `serversChanged`.
     */
    Q_INVOKABLE bool remove(const QString& slug);

    /**
     * @brief Triggers an asynchronous model-list refresh for the
     *        given server.
     * @param slug Existing slug; no-op when unknown.
     *
     * The dispatch is fire-and-forget. The response arrives on
     * `OpenAICompatProvider::modelsRefreshed`, which ModelRouter
     * forwards as `modelsRefreshed(providerId, models)`.  Callers
     * already subscribed to ModelRouter or `AgentSettingsController`
     * see the result without any additional wiring.
     */
    Q_INVOKABLE void refreshModels(const QString& slug);

    /**
     * @brief Probes a candidate server configuration WITHOUT mutating
     *        any persisted state.  Used by the QML setup sheet's
     *        "Test Connection" button to validate that what the user
     *        has typed will work BEFORE they commit it.
     *
     * @param correlationToken Caller-supplied identifier that the
     *        signal echoes back so QML can route the result to the
     *        right setup-sheet instance.  For an existing-server
     *        edit flow, the slug is a natural choice; for a brand-
     *        new server being authored, any unique string works.
     * @param baseUrl     Candidate base URL ending in `/v1` for most
     *                    stacks.
     * @param requiresKey When true, ensures the Authorization header
     *                    is sent even if `apiKey` is empty (the test
     *                    explicitly probes the auth-required path).
     * @param apiKey      Candidate API key, or empty for the
     *                    auth-less probe.
     *
     * Classifies the response into one of seven verdict keys:
     *  - `unreachable`:       TCP / DNS / refused at network level.
     *  - `auth_rejected`:     HTTP 401 with an API key sent.
     *  - `auth_required`:     HTTP 401 with no API key sent.
     *  - `path_not_found`:    HTTP 404 (URL likely missing `/v1`).
     *  - `server_error`:      HTTP 5xx.
     *  - `no_models_loaded`:  HTTP 200 with empty `data: []`.
     *  - `ok`:                HTTP 200 with at least one model id.
     *
     * The result arrives asynchronously on
     * `connectionTestResult(correlationToken, ok, verdictKey,
     * humanMessage, models)`.  The `models` list is populated only
     * on the `ok` verdict; the QML setup sheet can pre-populate the
     * model dropdown immediately so the "model list never empty"
     * rule holds even before the configuration is saved.
     */
    Q_INVOKABLE void testConnection(const QString& correlationToken,
                                    const QString& baseUrl,
                                    bool requiresKey,
                                    const QString& apiKey);

  signals:
    /**
     * @brief Emitted after every add / update / remove that lands.
     *        QML bindings on `list()` recompute on receipt.
     */
    void serversChanged();

    /**
     * @brief Emitted after a `testConnection` probe completes,
     *        either by HTTP response or network-level failure.
     * @param correlationToken  The token the caller passed to
     *                          `testConnection`; lets QML route the
     *                          result back to the right setup-sheet
     *                          instance.
     * @param ok                true iff the verdict is `ok`.
     * @param verdictKey        One of the seven documented verdict
     *                          keys: unreachable / auth_rejected /
     *                          auth_required / path_not_found /
     *                          server_error / no_models_loaded / ok.
     * @param humanMessage      User-facing explanation suitable for
     *                          rendering directly in the setup-sheet
     *                          status pill.
     * @param models            Model id list parsed from a successful
     *                          `/v1/models` response; empty on every
     *                          non-`ok` verdict.  QML can pre-populate
     *                          the model dropdown from this so the
     *                          "model list never empty" rule holds
     *                          before the configuration is saved.
     */
    void connectionTestResult(const QString& correlationToken,
                              bool ok,
                              const QString& verdictKey,
                              const QString& humanMessage,
                              const QStringList& models);

  private:
    /**
     * @brief Per-server runtime state.  HttpClient owned here so
     *        per-instance isolation is preserved; the OpenAICompatProvider
     *        pointer is non-owning (ModelRouter owns the unique_ptr).
     */
    struct InstanceRow {
        QString slug;
        QString displayName;
        QString baseUrl;
        bool requiresApiKeyFlag = false;
        bool supportsStreaming = true;
        bool supportsToolCalling = true;
        bool supportsVision = false;
        QString lastRefreshStatus = QStringLiteral("pending");
        std::unique_ptr<HttpClient> http;
        QPointer<OpenAICompatProvider> instance;
    };

    ModelRouter& m_router;
    SettingsService& m_settings;
    std::vector<InstanceRow> m_rows;

    void hydrate();
    void persist();
    QString providerIdFor(const QString& slug) const;
    QString generateSlug(const QString& fromDisplayName) const;
    static bool isValidSlug(const QString& slug);
    /// Canonical form of a base URL for duplicate detection: trimmed,
    /// lowercase scheme + host, trailing slash dropped (path/port kept).
    static QString normalizeBaseUrl(const QString& raw);
    InstanceRow* rowBySlug(const QString& slug);
    const InstanceRow* rowBySlug(const QString& slug) const;
    /**
     * @brief Constructs the HttpClient + OpenAICompatProvider for a
     *        row, applies all fields + API key, registers with
     *        ModelRouter, kicks off `refreshModels`.
     * @param row     Row to attach.  Mutates `row.http` +
     *                `row.instance`.
     * @param apiKey  API key value to apply to the provider.  Empty
     *                string means keyless.
     * @returns true on success; false when ModelRouter rejected the
     *          registration (id collision).
     */
    bool buildAndRegister(InstanceRow& row, const QString& apiKey);
    QVariantMap rowToVariantMap(const InstanceRow& row) const;
};
