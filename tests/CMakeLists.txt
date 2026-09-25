# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later


find_package(Qt6 REQUIRED COMPONENTS Test)

function(add_unit_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        verzeta-studio-backend
        Qt6::Test
        Qt6::Sql
        Qt6::Network
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL!"
    )
endfunction()

function(add_integration_test name)
    set(sources ${ARGN})
    add_executable(${name} ${sources})
    target_link_libraries(${name} PRIVATE
        verzeta-studio-backend
        Qt6::Test
        Qt6::Sql
        Qt6::Network
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL!"
        LABELS "integration"
    )
endfunction()

add_unit_test(test-web-search            unit/test-web-search.cpp)
add_unit_test(test-contention-ledger     unit/test-contention-ledger.cpp)
add_unit_test(test-infer-protocol        unit/test-infer-protocol.cpp)

if(TARGET verzeta-inference)
    add_integration_test(test-inference-sidecar
                         integration/test-inference-sidecar.cpp)
    target_compile_definitions(test-inference-sidecar PRIVATE
        VERZETA_INFERENCE_BIN="$<TARGET_FILE:verzeta-inference>")
    add_dependencies(test-inference-sidecar verzeta-inference)

    add_integration_test(test-inference-sidecar-host
                         integration/test-inference-sidecar-host.cpp)
    target_compile_definitions(test-inference-sidecar-host PRIVATE
        VERZETA_INFERENCE_BIN="$<TARGET_FILE:verzeta-inference>")
    add_dependencies(test-inference-sidecar-host verzeta-inference)
endif()
add_unit_test(test-db-manager           unit/test-db-manager.cpp)
add_unit_test(test-conversation-service unit/test-conversation-service.cpp)
add_unit_test(test-openai-compat-message-normalizer unit/test-openai-compat-message-normalizer.cpp)
add_unit_test(test-message-service      unit/test-message-service.cpp)

add_unit_test(test-filesystem-check
    unit/test-filesystem-check.cpp)
add_unit_test(test-command-line-options
    unit/test-command-line-options.cpp)

add_unit_test(test-folder-mount-registry
    unit/test-folder-mount-registry.cpp)

find_package(Qt6 REQUIRED COMPONENTS WebSockets)
add_executable(test-wire-session-client-rpc
    unit/test-wire-session-client-rpc.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-session.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-auth.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-host-client.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-db-reader.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-server.cpp
)
target_link_libraries(test-wire-session-client-rpc PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
)
target_include_directories(test-wire-session-client-rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-session-client-rpc PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-session-client-rpc
    COMMAND test-wire-session-client-rpc)
set_tests_properties(test-wire-session-client-rpc PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-wire-session-auth-capabilities
    unit/test-wire-session-auth-capabilities.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-session.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-auth.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-host-client.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-db-reader.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-server.cpp
)
target_link_libraries(test-wire-session-auth-capabilities PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
)
target_include_directories(test-wire-session-auth-capabilities PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-session-auth-capabilities PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-session-auth-capabilities
    COMMAND test-wire-session-auth-capabilities)
set_tests_properties(test-wire-session-auth-capabilities PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-wire-session-canvas-stdin
    unit/test-wire-session-canvas-stdin.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-session.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-auth.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-host-client.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-db-reader.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-server.cpp
)
target_link_libraries(test-wire-session-canvas-stdin PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
)
target_include_directories(test-wire-session-canvas-stdin PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-session-canvas-stdin PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-session-canvas-stdin
    COMMAND test-wire-session-canvas-stdin)
set_tests_properties(test-wire-session-canvas-stdin PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-path-utils
    unit/test-path-utils.cpp
    ${CMAKE_SOURCE_DIR}/backend/utils/path-utils.cpp
)
target_link_libraries(test-path-utils PRIVATE Qt6::Test Qt6::Core)
target_include_directories(test-path-utils PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-path-utils PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-path-utils COMMAND test-path-utils)
set_tests_properties(test-path-utils PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-voice-call-service
    unit/test-voice-call-service.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-call-service.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-sidecar-host.cpp
    ${CMAKE_SOURCE_DIR}/backend/voice/voice-text.cpp
    ${CMAKE_SOURCE_DIR}/backend/inference/infer-protocol.cpp
)
target_link_libraries(test-voice-call-service PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
target_include_directories(test-voice-call-service PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-voice-call-service PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-voice-call-service COMMAND test-voice-call-service)
set_tests_properties(test-voice-call-service PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-voice-turn-gate
    unit/test-voice-turn-gate.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-call-service.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-sidecar-host.cpp
    ${CMAKE_SOURCE_DIR}/backend/voice/voice-text.cpp
    ${CMAKE_SOURCE_DIR}/backend/inference/infer-protocol.cpp
)
target_link_libraries(test-voice-turn-gate PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
target_include_directories(test-voice-turn-gate PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-voice-turn-gate PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-voice-turn-gate COMMAND test-voice-turn-gate)
set_tests_properties(test-voice-turn-gate PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-voice-text
    unit/test-voice-text.cpp
    ${CMAKE_SOURCE_DIR}/backend/voice/voice-text.cpp
)
target_link_libraries(test-voice-text PRIVATE Qt6::Test Qt6::Core)
target_include_directories(test-voice-text PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-voice-text PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-voice-text COMMAND test-voice-text)
set_tests_properties(test-voice-text PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-voice-model-downloader
    unit/test-voice-model-downloader.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-model-downloader.cpp
    ${CMAKE_SOURCE_DIR}/backend/voice/voice-model-catalog.cpp
)
target_link_libraries(test-voice-model-downloader PRIVATE
    Qt6::Test Qt6::Core Qt6::Network)
target_include_directories(test-voice-model-downloader PRIVATE
    ${CMAKE_SOURCE_DIR}/backend)
set_target_properties(test-voice-model-downloader PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-voice-model-downloader
         COMMAND test-voice-model-downloader)
set_tests_properties(test-voice-model-downloader PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(fake-voice-daemon
    helpers/fake-voice-daemon.cpp
    ${CMAKE_SOURCE_DIR}/backend/inference/infer-protocol.cpp
)
target_link_libraries(fake-voice-daemon PRIVATE Qt6::Core)
target_include_directories(fake-voice-daemon PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
)

add_executable(test-voice-lifecycle
    integration/test-voice-lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-call-service.cpp
    ${CMAKE_SOURCE_DIR}/backend/services/voice-sidecar-host.cpp
    ${CMAKE_SOURCE_DIR}/backend/voice/voice-text.cpp
    ${CMAKE_SOURCE_DIR}/backend/inference/infer-protocol.cpp
)
target_link_libraries(test-voice-lifecycle PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
target_include_directories(test-voice-lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-voice-lifecycle PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(test-voice-lifecycle fake-voice-daemon)
add_test(NAME test-voice-lifecycle COMMAND test-voice-lifecycle)
set_tests_properties(test-voice-lifecycle PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!"
    LABELS "integration"
    TIMEOUT 120)

add_executable(test-wire-host-client-exec-gate
    unit/test-wire-host-client-exec-gate.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-session.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-auth.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-host-client.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-db-reader.cpp
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-server.cpp
)
target_link_libraries(test-wire-host-client-exec-gate PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
)
target_include_directories(test-wire-host-client-exec-gate PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-host-client-exec-gate PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-host-client-exec-gate
    COMMAND test-wire-host-client-exec-gate)
set_tests_properties(test-wire-host-client-exec-gate PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-wire-host-bridge-client-rpc
    unit/test-wire-host-bridge-client-rpc.cpp
)
target_link_libraries(test-wire-host-bridge-client-rpc PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Network
)
target_include_directories(test-wire-host-bridge-client-rpc PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-host-bridge-client-rpc PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-host-bridge-client-rpc
    COMMAND test-wire-host-bridge-client-rpc)
set_tests_properties(test-wire-host-bridge-client-rpc PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-wire-invoke-conv-delete
    integration/test-wire-invoke-conv-delete.cpp
)
target_link_libraries(test-wire-invoke-conv-delete PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Network
)
target_include_directories(test-wire-invoke-conv-delete PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-invoke-conv-delete PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-invoke-conv-delete
    COMMAND test-wire-invoke-conv-delete)
set_tests_properties(test-wire-invoke-conv-delete PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-wire-search-providers
    integration/test-wire-search-providers.cpp
)
target_link_libraries(test-wire-search-providers PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Network
)
target_include_directories(test-wire-search-providers PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-wire-search-providers PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-wire-search-providers
    COMMAND test-wire-search-providers)
set_tests_properties(test-wire-search-providers PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")


add_executable(test-folder-mount-client-wire
    unit/test-folder-mount-client-wire.cpp
)
target_link_libraries(test-folder-mount-client-wire PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Network
    Qt6::Sql
)
target_include_directories(test-folder-mount-client-wire PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-folder-mount-client-wire PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-folder-mount-client-wire
    COMMAND test-folder-mount-client-wire)
set_tests_properties(test-folder-mount-client-wire PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-virtual-fs-router
    unit/test-virtual-fs-router.cpp
)
target_link_libraries(test-virtual-fs-router PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Sql
)
target_include_directories(test-virtual-fs-router PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-virtual-fs-router PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-virtual-fs-router
    COMMAND test-virtual-fs-router)
set_tests_properties(test-virtual-fs-router PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-virtual-fs-roundtrip
    integration/test-virtual-fs-roundtrip.cpp
)
target_link_libraries(test-virtual-fs-roundtrip PRIVATE
    verzeta-studio-backend
    Qt6::Test
    Qt6::Core
    Qt6::Sql
)
target_include_directories(test-virtual-fs-roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
    ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(test-virtual-fs-roundtrip PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
add_test(NAME test-virtual-fs-roundtrip
    COMMAND test-virtual-fs-roundtrip)
set_tests_properties(test-virtual-fs-roundtrip PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL!")

add_executable(test-db-lock-helper helpers/db-lock-test-helper.cpp)
target_link_libraries(test-db-lock-helper PRIVATE
    verzeta-studio-backend
    Qt6::Core
    Qt6::Sql
)
target_include_directories(test-db-lock-helper PRIVATE
    ${CMAKE_SOURCE_DIR}/backend
)
set_target_properties(test-db-lock-helper PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

add_integration_test(test-db-lock-refusal
    integration/test-db-lock-refusal.cpp)
add_dependencies(test-db-lock-refusal test-db-lock-helper)

add_unit_test(test-message-service-get-message
    unit/test-message-service-get-message.cpp)

add_unit_test(test-model-router         unit/test-model-router.cpp)

add_unit_test(test-new-providers
    unit/test-new-providers.cpp)

add_unit_test(test-context-window
    unit/test-context-window.cpp)

add_unit_test(test-model-router-dual-slot
    unit/test-model-router-dual-slot.cpp)

add_integration_test(test-model-router-characterization
    integration/test-model-router-characterization.cpp)

add_unit_test(test-heartbeat-schedule
    unit/test-heartbeat-schedule.cpp)
add_unit_test(test-heartbeat-report-parser
    unit/test-heartbeat-report-parser.cpp)
add_unit_test(test-heartbeat-subagent-queue
    unit/test-heartbeat-subagent-queue.cpp)

add_integration_test(test-heartbeat-config-service
    integration/test-heartbeat-config-service.cpp)
add_integration_test(test-heartbeat-multi-alias-scoping
    integration/test-heartbeat-multi-alias-scoping.cpp)
add_integration_test(test-heartbeat-tier1-async-dispatch
    integration/test-heartbeat-tier1-async-dispatch.cpp)

add_integration_test(test-heartbeat-tool-access
    integration/test-heartbeat-tool-access.cpp)

add_unit_test(test-skill-hash               unit/test-skill-hash.cpp)
add_unit_test(test-skill-parser             unit/test-skill-parser.cpp)

add_unit_test(test-dangerous-pattern-scanner
    unit/test-dangerous-pattern-scanner.cpp)
add_unit_test(test-model-sampling-profile
    unit/test-model-sampling-profile.cpp)
add_integration_test(test-skill-service
    integration/test-skill-service.cpp)
add_integration_test(test-skill-preferred-lists
    integration/test-skill-preferred-lists.cpp)

add_integration_test(test-request-builder-skills
    integration/test-request-builder-skills.cpp)

add_integration_test(test-request-builder-tool-scope
    integration/test-request-builder-tool-scope.cpp)

add_integration_test(test-request-builder-payload-fit
    integration/test-request-builder-payload-fit.cpp)

add_integration_test(test-per-agent-provider
    integration/test-per-agent-provider.cpp)
add_integration_test(test-skill-tools
    integration/test-skill-tools.cpp)

add_integration_test(test-get-skill-status-tool
    integration/test-get-skill-status-tool.cpp)

add_integration_test(test-poll-service
    integration/test-poll-service.cpp)

add_integration_test(test-poll-tools
    integration/test-poll-tools.cpp)

add_integration_test(test-heartbeat-skill-integration
    integration/test-heartbeat-skill-integration.cpp)

add_integration_test(test-skill-archive-extractor
    integration/test-skill-archive-extractor.cpp)

add_integration_test(test-membership-tools
    integration/test-membership-tools.cpp)

add_integration_test(test-membership-service
    integration/test-membership-service.cpp)

add_integration_test(test-generate-image-tool
    integration/test-generate-image-tool.cpp)

add_integration_test(test-image-job-attribution
    integration/test-image-job-attribution.cpp)

add_integration_test(test-intervening-system-message
    integration/test-intervening-system-message.cpp)

add_integration_test(test-cap-hit-cascade-finalize
    integration/test-cap-hit-cascade-finalize.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-user-message-preemption
    integration/test-user-message-preemption.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-conversation-run-characterization
    integration/test-conversation-run-characterization.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-chat-coordinator
    integration/test-chat-coordinator.cpp
    helpers/scripted-mock-provider.cpp)

add_unit_test(test-provider-scheduler
    unit/test-provider-scheduler.cpp)

add_integration_test(test-concurrent-multichat
    integration/test-concurrent-multichat.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-concurrent-agent-multichat
    integration/test-concurrent-agent-multichat.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-multichat-generating-ui
    integration/test-multichat-generating-ui.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-multichat-live-streaming
    integration/test-multichat-live-streaming.cpp
    helpers/scripted-mock-provider.cpp)

add_integration_test(test-walk-and-pair-permutations
    integration/test-walk-and-pair-permutations.cpp)

add_integration_test(test-cascade-multi-agent-scenario
    integration/test-cascade-multi-agent-scenario.cpp)

add_integration_test(test-cross-session-cascade-isolation
    integration/test-cross-session-cascade-isolation.cpp)

add_integration_test(test-cross-cascade-self-echo
    integration/test-cross-cascade-self-echo.cpp)

add_integration_test(test-audit-service
    integration/test-audit-service.cpp)

add_integration_test(test-project-template-service
    integration/test-project-template-service.cpp)

add_integration_test(test-heartbeat-auto-surface-gate
    integration/test-heartbeat-auto-surface-gate.cpp)

add_integration_test(test-heartbeat-tier2-review
    integration/test-heartbeat-tier2-review.cpp)
add_integration_test(test-heartbeat-manual-override
    integration/test-heartbeat-manual-override.cpp)
add_integration_test(test-heartbeat-stream-collision
    integration/test-heartbeat-stream-collision.cpp)

add_integration_test(test-heartbeat-folder-scope
    integration/test-heartbeat-folder-scope.cpp)

add_integration_test(test-heartbeat-crash-restart
    integration/test-heartbeat-crash-restart.cpp)

add_integration_test(test-heartbeat-self-config-tools
    integration/test-heartbeat-self-config-tools.cpp)

add_integration_test(test-ollama-provider   integration/test-ollama-provider.cpp)
add_integration_test(test-openai-provider   integration/test-openai-provider.cpp)

add_unit_test(test-openai-requires-api-key
    unit/test-openai-requires-api-key.cpp)

add_integration_test(test-llamacpp-remote-empty-key-dispatch
    integration/test-llamacpp-remote-empty-key-dispatch.cpp)

add_unit_test(test-openai-compat-provider
    unit/test-openai-compat-provider.cpp)

add_integration_test(test-custom-server-registry
    integration/test-custom-server-registry.cpp)

add_integration_test(test-image-provider-registry
    integration/test-image-provider-registry.cpp)

add_integration_test(test-custom-server-test-connection
    integration/test-custom-server-test-connection.cpp)

add_integration_test(test-openai-compat-fixture-replay
    integration/test-openai-compat-fixture-replay.cpp)

add_unit_test(test-message-list-model unit/test-message-list-model.cpp)
add_unit_test(test-markdown-utils     unit/test-markdown-utils.cpp)
add_unit_test(test-escaped-text-repair unit/test-escaped-text-repair.cpp)
add_unit_test(test-logger-rotation     unit/test-logger-rotation.cpp)

add_unit_test(test-export-service unit/test-export-service.cpp)
add_unit_test(test-conversation-list-model unit/test-conversation-list-model.cpp)
add_unit_test(test-plans-model             unit/test-plans-model.cpp)
add_unit_test(test-tool-call-log-model     unit/test-tool-call-log-model.cpp)
add_unit_test(test-artifacts-model         unit/test-artifacts-model.cpp)
add_unit_test(test-message-streaming       unit/test-message-streaming.cpp)
add_unit_test(test-sidebar-flat-model      unit/test-sidebar-flat-model.cpp)

add_integration_test(test-conversation-deletion-cascade
                      integration/test-conversation-deletion-cascade.cpp)

add_integration_test(test-canvas-service-schema
                      integration/test-canvas-service-schema.cpp)

add_integration_test(test-canvas-service-roundtrip
                      integration/test-canvas-service-roundtrip.cpp)

add_integration_test(test-canvas-tool-read
                      integration/test-canvas-tool-read.cpp)

add_integration_test(test-canvas-tool-edit
                      integration/test-canvas-tool-edit.cpp)

add_integration_test(test-canvas-tool-per-client
                      integration/test-canvas-tool-per-client.cpp)

add_integration_test(test-poll-tool-per-client
                      integration/test-poll-tool-per-client.cpp)

add_integration_test(test-membership-tool-per-client
                      integration/test-membership-tool-per-client.cpp)

add_integration_test(test-heartbeat-tool-per-client
                      integration/test-heartbeat-tool-per-client.cpp)

add_integration_test(test-skill-tool-per-client
                      integration/test-skill-tool-per-client.cpp)

add_integration_test(test-request-builder-canvas-prompt
                      integration/test-request-builder-canvas-prompt.cpp)

add_integration_test(test-request-builder-task-prompt
                      integration/test-request-builder-task-prompt.cpp)

add_integration_test(test-canvas-metadata-cache-mirror
                      integration/test-canvas-metadata-cache-mirror.cpp)

add_integration_test(test-canvas-tools-actions
                      integration/test-canvas-tools-actions.cpp)

add_integration_test(test-canvas-auto-promote
                      integration/test-canvas-auto-promote.cpp)

add_integration_test(test-canvas-switch
                      integration/test-canvas-switch.cpp)

add_integration_test(test-conversation-pinning
                      integration/test-conversation-pinning.cpp)

add_integration_test(test-project-direct-chat-folder
                      integration/test-project-direct-chat-folder.cpp)

add_integration_test(test-sidebar-sections-model
                      integration/test-sidebar-sections-model.cpp)

add_integration_test(test-canvas-export
                      integration/test-canvas-export.cpp)

add_unit_test(test-canvas-console-model unit/test-canvas-console-model.cpp)

add_integration_test(test-canvas-runner
                      integration/test-canvas-runner.cpp)

add_integration_test(test-canvas-ai-actions
                      integration/test-canvas-ai-actions.cpp)

add_integration_test(test-chat-controller-empty-finish
                      integration/test-chat-controller-empty-finish.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-request-builder
                      integration/test-request-builder.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-request-builder-routing
                      integration/test-request-builder-routing.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-request-builder-no-thinking-leak
                      integration/test-request-builder-no-thinking-leak.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-streaming-manager
                      integration/test-streaming-manager.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-tool-dispatcher
                      integration/test-tool-dispatcher.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-cascade-controller
                      integration/test-cascade-controller.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-cascade-default-bias
                      integration/test-cascade-default-bias.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-task-gate-service
                      integration/test-task-gate-service.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-history-assembly
                      integration/test-history-assembly.cpp)

add_integration_test(test-agent-pattern-missing-model-guard
                      integration/test-agent-pattern-missing-model-guard.cpp
                      helpers/scripted-mock-provider.cpp)
add_integration_test(test-agent-pattern-prompt-parity
                      integration/test-agent-pattern-prompt-parity.cpp
                      helpers/scripted-mock-provider.cpp)
add_integration_test(test-agent-pattern-cross-chat-awareness
                      integration/test-agent-pattern-cross-chat-awareness.cpp
                      helpers/scripted-mock-provider.cpp)
add_integration_test(test-agent-pattern-attachment-parity
                      integration/test-agent-pattern-attachment-parity.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-agent-chunk-dispatch
                      integration/test-agent-chunk-dispatch.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-task-controller
                      integration/test-task-controller.cpp
                      helpers/scripted-mock-provider.cpp)

add_integration_test(test-tool-service-registration-order
                      integration/test-tool-service-registration-order.cpp)

add_unit_test(test-file-service unit/test-file-service.cpp)

add_integration_test(test-tool-execution integration/test-tool-execution.cpp)

add_unit_test(test-tool-service unit/test-tool-service.cpp)
add_unit_test(test-rag-service  unit/test-rag-service.cpp)
add_unit_test(test-embedding-worker unit/test-embedding-worker.cpp)
add_unit_test(test-vector-store unit/test-vector-store.cpp)
if(SQLITE_VEC_LOADABLE)
    target_compile_definitions(test-vector-store
        PRIVATE VERZETA_VEC0_PATH="${SQLITE_VEC_LOADABLE}")
endif()

add_unit_test(test-vector-store-service unit/test-vector-store-service.cpp)
if(SQLITE_VEC_LOADABLE)
    target_compile_definitions(test-vector-store-service
        PRIVATE VERZETA_VEC0_PATH="${SQLITE_VEC_LOADABLE}")
endif()

add_unit_test(test-agent-memory-service unit/test-agent-memory-service.cpp)

add_unit_test(test-acn-service unit/test-acn-service.cpp)

add_unit_test(test-remember-tool unit/test-remember-tool.cpp)

add_unit_test(test-memory-cascade-delete unit/test-memory-cascade-delete.cpp)

add_unit_test(test-memory-retriever unit/test-memory-retriever.cpp)

add_unit_test(test-content-sanitizer unit/chat/test-content-sanitizer.cpp)

add_unit_test(test-claims-detector unit/chat/test-claims-detector.cpp)
add_unit_test(test-action-intent-confirmer unit/chat/test-action-intent-confirmer.cpp)

add_unit_test(test-conversation-summarizer unit/test-conversation-summarizer.cpp)

add_unit_test(test-llm-config unit/test-llm-config.cpp)

add_unit_test(test-slash-command-compact unit/test-slash-command-compact.cpp)

add_unit_test(test-subagent-run-service unit/test-subagent-run-service.cpp)

add_unit_test(test-inline-thinking-hoister
              unit/chat/test-inline-thinking-hoister.cpp)

add_unit_test(test-slash-command-service unit/test-slash-command-service.cpp)

add_unit_test(test-run-shell-tool    unit/tools/test-run-shell-tool.cpp)
add_unit_test(test-current-time-tool unit/tools/test-current-time-tool.cpp)
add_unit_test(test-request-turn-tool unit/tools/test-request-turn-tool.cpp)
add_unit_test(test-list-files-tool   unit/tools/test-list-files-tool.cpp)
add_unit_test(test-read-file-tool    unit/tools/test-read-file-tool.cpp)
add_unit_test(test-write-file-tool   unit/tools/test-write-file-tool.cpp)
add_unit_test(test-search-web-tool   unit/tools/test-search-web-tool.cpp)
add_unit_test(test-search-messages-tool
    unit/tools/test-search-messages-tool.cpp)
add_unit_test(test-read-conversation-tool
    unit/tools/test-read-conversation-tool.cpp)
add_unit_test(test-list-project-conversations-tool
    unit/tools/test-list-project-conversations-tool.cpp)

add_integration_test(test-rag-pipeline integration/test-rag-pipeline.cpp)
add_integration_test(test-memory-tool-handlers
                      integration/test-memory-tool-handlers.cpp)
add_integration_test(test-task-tool-handlers
                      integration/test-task-tool-handlers.cpp)
add_integration_test(test-conversation-manager
                      integration/test-conversation-manager.cpp
                      helpers/scripted-mock-provider.cpp)
add_integration_test(test-request-id-gating
                      integration/test-request-id-gating.cpp
                      helpers/scripted-mock-provider.cpp)
add_integration_test(test-ragp-pipeline-e2e
                      integration/test-ragp-pipeline-e2e.cpp
                      helpers/scripted-mock-provider.cpp)

add_unit_test(test-agent-service unit/test-agent-service.cpp)

add_unit_test(test-image-service unit/test-image-service.cpp)
add_unit_test(test-audio-service unit/test-audio-service.cpp)

add_integration_test(test-multimedia-pipeline integration/test-multimedia-pipeline.cpp)

add_unit_test(test-search-service unit/test-search-service.cpp)

add_unit_test(test-plan-service unit/test-plan-service.cpp)
add_unit_test(test-task-observer unit/test-task-observer.cpp)

add_unit_test(test-history-budgeter unit/test-history-budgeter.cpp)

add_unit_test(test-tool-payload-digest unit/test-tool-payload-digest.cpp)

add_unit_test(test-history-tool-digest unit/test-history-tool-digest.cpp)

add_unit_test(test-ragp-rule-classifier unit/test-ragp-rule-classifier.cpp)
add_unit_test(test-ragp-cache unit/test-ragp-cache.cpp)
add_unit_test(test-ragp-service unit/test-ragp-service.cpp)
add_unit_test(test-ragp-remote-backend unit/test-ragp-remote-backend.cpp)

add_unit_test(test-settings-service unit/test-settings-service.cpp)
add_unit_test(test-model-download-service unit/test-model-download-service.cpp)
add_unit_test(test-background-process-service unit/test-background-process-service.cpp)

add_unit_test(test-local-llama-backend unit/test-local-llama-backend.cpp)

add_unit_test(test-ragp-backend-selector unit/test-ragp-backend-selector.cpp)

if(VERZETA_ENABLE_LLAMACPP)
    add_integration_test(test-ragp-backend-swap
                          integration/test-ragp-backend-swap.cpp)
endif()


add_integration_test(test-session-router
    integration/test-session-router.cpp)

add_integration_test(test-wire-db-reader-llm-config
    integration/test-wire-db-reader-llm-config.cpp)
target_sources(test-wire-db-reader-llm-config PRIVATE
    ${CMAKE_SOURCE_DIR}/backend/remote/server/wire-db-reader.cpp)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stress/stress-group-chat.cpp")
    add_executable(stress-group-chat stress/stress-group-chat.cpp)
    target_link_libraries(stress-group-chat PRIVATE
        verzeta-studio-backend
        Qt6::Core
        Qt6::Sql
        Qt6::Network)
    target_include_directories(stress-group-chat PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(stress-group-chat PRIVATE
        STRESS_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stress/scenarios")
    set_target_properties(stress-group-chat PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stress/stress-1to1-chat.cpp")
    add_executable(stress-1to1-chat stress/stress-1to1-chat.cpp)
    target_link_libraries(stress-1to1-chat PRIVATE
        verzeta-studio-backend
        Qt6::Core
        Qt6::Sql
        Qt6::Network)
    target_include_directories(stress-1to1-chat PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(stress-1to1-chat PRIVATE
        STRESS_SCENARIO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stress/scenarios")
    set_target_properties(stress-1to1-chat PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stress/stress-group-chat-multi-provider.cpp")
    add_executable(stress-group-chat-multi-provider
        stress/stress-group-chat-multi-provider.cpp)
    target_link_libraries(stress-group-chat-multi-provider PRIVATE
        verzeta-studio-backend
        Qt6::Core
        Qt6::Sql
        Qt6::Network)
    target_include_directories(stress-group-chat-multi-provider PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(stress-group-chat-multi-provider
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stress/stress-concurrent-multichat.cpp")
    add_executable(stress-concurrent-multichat
        stress/stress-concurrent-multichat.cpp)
    target_link_libraries(stress-concurrent-multichat PRIVATE
        verzeta-studio-backend
        Qt6::Core
        Qt6::Sql
        Qt6::Network)
    target_include_directories(stress-concurrent-multichat PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(stress-concurrent-multichat
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stress/stress-remote-lifecycle.cpp")
    add_executable(stress-remote-lifecycle
        stress/stress-remote-lifecycle.cpp)
    target_link_libraries(stress-remote-lifecycle PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Sql)
    target_include_directories(stress-remote-lifecycle PRIVATE
        ${CMAKE_SOURCE_DIR}/backend
        ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(stress-remote-lifecycle
        PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_dependencies(stress-remote-lifecycle verzeta-studio verzeta-remote)
endif()
