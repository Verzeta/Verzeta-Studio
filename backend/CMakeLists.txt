# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: LGPL-3.0-or-later


add_library(verzeta-studio-backend STATIC
    models/db-manager.cpp
    models/conversation.cpp
    models/message.cpp
    models/attachment.cpp
    models/tool-call.cpp
    models/llm-config.cpp
    models/user-profile.cpp
    models/job-context.cpp
    models/activity-event.cpp
    services/conversation-service.cpp
    services/message-service.cpp
    services/audit-service.cpp
    services/folder-mount-registry.cpp
    services/folder-mount-client-wire.cpp
    services/help-service.cpp
    services/project-template-service.cpp
    utils/logger.cpp
    utils/contention-ledger.cpp
    inference/infer-protocol.cpp
    services/inference-sidecar-host.cpp
    utils/json-utils.cpp
    utils/escaped-text-repair.cpp
    utils/crypto-utils.cpp
    utils/filesystem-check.cpp
    utils/path-utils.cpp
    utils/command-line-options.cpp
    app-controller.cpp

    utils/http-client.cpp
    api/llm-interface.cpp
    api/ollama-provider.cpp
    api/openai-provider.cpp
    api/anthropic-provider.cpp
    api/gemini-provider.cpp

    api/openrouter-provider.cpp
    api/deepseek-provider.cpp
    api/llamacpp-remote-provider.cpp
    api/openai-compat-provider.cpp
    services/custom-server-registry.cpp
    services/image-provider-registry.cpp
    services/search/web-search-service.cpp
    services/search/web-search-provider-registry.cpp
    services/search/web-search-http.cpp
    services/search/ddg-html-provider.cpp
    services/search/ddg-instant-answer-provider.cpp
    services/search/tavily-provider.cpp
    services/search/exa-provider.cpp
    services/search/langsearch-provider.cpp
    services/search/searxng-provider.cpp
    services/search/custom-json-provider.cpp
    api/llamacpp-provider.cpp
    services/model-router.cpp
    services/model-sampling-profile.cpp
    services/settings-service.cpp
    services/model-download-service.cpp
    services/voice-call-service.cpp
    services/voice-sidecar-host.cpp
    services/voice-model-downloader.cpp
    voice/voice-model-catalog.cpp
    voice/voice-text.cpp
    services/background-process-service.cpp

    models/message-list-model.cpp
    utils/markdown-utils.cpp
    services/chat-controller.cpp
    services/history-budgeter.cpp

    services/chat/request-builder.cpp
    services/chat/streaming-manager.cpp
    services/chat/tool-dispatcher.cpp
    services/chat/cascade-controller.cpp
    services/chat/conversation-run.cpp
    services/chat/provider-scheduler.cpp
    services/chat/memory-retriever.cpp
    services/chat/rag-source.cpp
    services/chat/aim-source.cpp
    services/chat/acn-source.cpp
    services/chat/action-intent-confirmer.cpp
    services/task-gate-service.cpp
    services/chat/content-sanitizer.cpp
    services/chat/inline-thinking-hoister.cpp
    services/chat/tool-payload-digest.cpp
    services/slash-command-service.cpp

    services/canvas-service.cpp

    tools/canvas/open-canvas-tool.cpp
    tools/canvas/read-canvas-tool.cpp
    tools/canvas/edit-canvas-tool.cpp
    utils/canvas-line-number-gutter.cpp

    services/canvas-runner.cpp
    services/canvas-ai-actions.cpp
    models/canvas-console-model.cpp

    services/ragp/ragp-types.cpp
    services/ragp/ragp-cache.cpp
    services/ragp/ragp-rule-classifier.cpp
    services/ragp/stub-ragp-backend.cpp
    services/ragp/remote-ragp-backend.cpp
    services/ragp/classifier-provider-factory.cpp
    services/conversation-summarizer.cpp
    services/subagent-run-service.cpp
    tools/subagent/subagent-tools.cpp
    services/ragp/ragp-service.cpp
    services/ragp/ragp-backend-selector.cpp
    services/ragp/local-llama-backend.cpp

    models/conversation-list-model.cpp
    models/plans-model.cpp
    models/tool-call-log-model.cpp
    models/artifacts-model.cpp
    models/sidebar-flat-model.cpp
    services/export-service.cpp

    services/conversation-controller.cpp

    services/task-controller.cpp

    services/agent-settings-controller.cpp

    services/session/session-router.cpp

    services/export-controller.cpp

    services/file-service.cpp
    utils/process-sandbox.cpp
    utils/build-info.cpp
    workers/tool-execution-worker.cpp

    api/tool-calling-schema.cpp
    services/tool-service.cpp
    services/mcp-client.cpp
    services/mcp-service.cpp
    workers/embedding-worker.cpp
    services/vector-store.cpp
    services/rag-service.cpp
    services/memory-entry-store.cpp
    services/agent-memory-service.cpp
    services/acn-service.cpp

    tools/tool-registration.cpp
    tools/itool-adapter.cpp
    tools/shell/run-shell-tool.cpp
    tools/time/current-time-tool.cpp
    tools/cascade/request-turn-tool.cpp
    tools/file/list-files-tool.cpp
    tools/file/read-file-tool.cpp
    tools/file/write-file-tool.cpp
    tools/web/search-web-tool.cpp
    tools/memory/search-messages-tool.cpp
    tools/memory/read-conversation-tool.cpp
    tools/memory/list-project-conversations-tool.cpp
    tools/memory/remember-tool.cpp

    services/agent-service.cpp
    workers/multi-agent-coordinator.cpp

    models/agent.cpp
    services/agent-registry.cpp

    models/member.cpp
    services/membership-service.cpp

    services/poll-service.cpp
    tools/polls/poll-tools.cpp

    models/agent-plan.cpp
    services/plan-service.cpp
    services/task-runner.cpp
    services/task-observer.cpp

    services/image-service.cpp
    services/audio-service.cpp
    workers/image-gen-worker.cpp
    workers/audio-gen-worker.cpp

    services/search-service.cpp
    utils/notification-manager.cpp

    models/heartbeat-config.cpp
    models/heartbeat-report.cpp
    utils/heartbeat-schedule.cpp
    utils/heartbeat-report-parser.cpp
    services/heartbeat-config-service.cpp
    services/heartbeat-subagent-queue.cpp
    services/heartbeat-subagent-service.cpp

    models/heartbeat-reports-model.cpp

    tools/heartbeat/heartbeat-self-config-tools.cpp

    utils/skill-hash.cpp
    utils/skill-parser.cpp
    services/skill-service.cpp
    models/skills-model.cpp

    tools/skills/discover-skills-tool.cpp
    tools/skills/get-skill-status-tool.cpp
    tools/skills/read-skill-file-tool.cpp
    tools/skills/read-skill-tool.cpp

    tools/membership/membership-tools.cpp

    tools/image/generate-image-tool.cpp

    services/clawhub-client.cpp
    utils/skill-archive-extractor.cpp

    utils/dangerous-pattern-scanner.cpp

    remote/wire-host-bridge.cpp
    remote/server/wire-auth.cpp
)

if(WIN32)
    target_sources(verzeta-studio-backend PRIVATE
        utils/windows-theme-bridge.cpp
    )
    target_link_libraries(verzeta-studio-backend PUBLIC
        KF6::BreezeIcons
        KF6::IconThemes
        KF6::ColorScheme
        KF6::ConfigCore
    )
endif()

target_link_libraries(verzeta-studio-backend PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Sql
    Qt6::Quick
    Qt6::Qml
    Qt6::Network
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::Widgets
    KF6::Notifications
    KF6::Archive
)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3)
endif()
if(TARGET PkgConfig::SQLITE3)
    target_link_libraries(verzeta-studio-backend PUBLIC PkgConfig::SQLITE3)
    target_compile_definitions(verzeta-studio-backend PUBLIC VERZETA_HAVE_SQLITE3=1)
    message(STATUS "sqlite-vec: system sqlite3 ${SQLITE3_VERSION} linked — runtime vector extension enabled")
else()
    message(WARNING "sqlite-vec: system sqlite3 not found via pkg-config; vector search falls back to brute-force.")
endif()



target_include_directories(verzeta-studio-backend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/generated/include
)

set_target_properties(verzeta-studio-backend PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

qt_add_resources(verzeta-studio-backend "project_templates"
    PREFIX "/project-templates"
    BASE   "${CMAKE_SOURCE_DIR}/resources/project-templates"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/project-templates/01-research-brief.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/02-customer-interview-synthesis.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/03-code-review-war-room.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/04-technical-spec.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/05-marketing-copy-studio.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/06-product-launch-plan.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/07-sales-pitch-workshop.json"
        "${CMAKE_SOURCE_DIR}/resources/project-templates/08-quarterly-business-review.json"
)


set(VERZETA_BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
        OUTPUT_VARIABLE _verzeta_commit_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _verzeta_commit_rc
    )
    if(_verzeta_commit_rc EQUAL 0 AND _verzeta_commit_out)
        set(VERZETA_BUILD_COMMIT "${_verzeta_commit_out}")
    endif()
endif()

target_compile_definitions(verzeta-studio-backend PUBLIC
    VERZETA_VERSION_STRING="${PROJECT_VERSION}"
    VERZETA_BUILD_COMMIT="${VERZETA_BUILD_COMMIT}"
    VERZETA_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

target_link_libraries(verzeta-studio-backend PUBLIC KF6::SyntaxHighlighting)

add_executable(verzeta-studio
    main.cpp
)

if(WIN32)
    target_sources(verzeta-studio PRIVATE
        ${CMAKE_SOURCE_DIR}/packaging/windows/verzeta-studio.rc
    )
    set_target_properties(verzeta-studio PROPERTIES WIN32_EXECUTABLE ON)
endif()

qt_add_resources(verzeta-studio "app_icons"
    PREFIX "/icons"
    BASE   "${CMAKE_SOURCE_DIR}/resources/icons"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/icons/verzeta-studio.svg"
        "${CMAKE_SOURCE_DIR}/resources/icons/verzeta-studio.png"
)

qt_add_resources(verzeta-studio "app_textures"
    PREFIX "/textures"
    BASE   "${CMAKE_SOURCE_DIR}/resources/textures"
    FILES
        "${CMAKE_SOURCE_DIR}/resources/textures/fabric.png"
        "${CMAKE_SOURCE_DIR}/resources/textures/groovepaper.png"
)

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/MainWindow.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/MainWindow.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/HomePage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/HomePage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/SettingsPanel.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/SettingsPanel.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/TextProvidersPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/TextProvidersPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/ImageProvidersPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/ImageProvidersPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/SearchProvidersPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/SearchProvidersPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/RagpProvidersPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/RagpProvidersPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/EmbeddingsProvidersPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/EmbeddingsProvidersPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProviderCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProviderCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CustomServerCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CustomServerCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CustomServerSetupSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CustomServerSetupSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ImageProviderSetupSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ImageProviderSetupSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/PollCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/PollCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/IconNavBar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/IconNavBar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/LeftSidebar.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/LeftSidebar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/ChatPanel.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/ChatPanel.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/RightSettingsPanel.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/RightSettingsPanel.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/CanvasPanel.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/CanvasPanel.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CanvasEditor.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CanvasEditor.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/ToolsPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/ToolsPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/AgentsPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/AgentsPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AppButton.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AppButton.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CallOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CallOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AppRadioButton.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AppRadioButton.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AppOverlayDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AppOverlayDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FrostedGlass.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FrostedGlass.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AppTextField.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AppTextField.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AppTextArea.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AppTextArea.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/RemoteAccessDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/RemoteAccessDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/VoiceCallsDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/VoiceCallsDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/RevokedDevicesSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/RevokedDevicesSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ExecutionPermissionsOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ExecutionPermissionsOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FeaturePill.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FeaturePill.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FolderSettingsDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FolderSettingsDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/NewGroupChatDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/NewGroupChatDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/MembershipEditor.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/MembershipEditor.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AddChatMemberDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AddChatMemberDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ModelOverrideEditor.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ModelOverrideEditor.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/SearchableModelComboBox.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/SearchableModelComboBox.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ChatInputBar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ChatInputBar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CompactionGauge.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CompactionGauge.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ModelSelectorBar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ModelSelectorBar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/SidebarConvItem.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/SidebarConvItem.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FolderSectionHeader.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FolderSectionHeader.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/SidebarSectionHeader.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/SidebarSectionHeader.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/SidebarConvContextMenu.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/SidebarConvContextMenu.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CanvasActionBar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CanvasActionBar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CanvasConsole.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CanvasConsole.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FirstRunWizard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FirstRunWizard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AboutDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AboutDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/PlansOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/PlansOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatActivityOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/HeartbeatActivityOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatDiagnosticsTab.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/HeartbeatDiagnosticsTab.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatAuditTab.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/HeartbeatAuditTab.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ActivityTimeline.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ActivityTimeline.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/HelpOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/HelpOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectRoomOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectTemplateCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectRoomCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateBanner.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectTemplateBanner.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectStatusPill.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectStatusPill.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectFilterPill.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectFilterPill.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AgentAvatar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AgentAvatar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomTexture.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectRoomTexture.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectQuickStartSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectQuickStartSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomKickoffSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectRoomKickoffSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateDetailSheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectTemplateDetailSheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateLibrarySheet.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ProjectTemplateLibrarySheet.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/SkillsPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/SkillsPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/SkillReviewDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/SkillReviewDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/PreferredSkillsOverlay.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/PreferredSkillsOverlay.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/MessageBubble.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/MessageBubble.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AssistantThinkingDisclosure.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AssistantThinkingDisclosure.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/CodeBlock.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/CodeBlock.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/MarkdownText.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/MarkdownText.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/TypingIndicator.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/TypingIndicator.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/StopRetryBar.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/StopRetryBar.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/TokenCounter.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/TokenCounter.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/StatChip.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/StatChip.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ToolActivityIndicator.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ToolActivityIndicator.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/hooks/ThemeController.qml
    PROPERTIES
        QT_RESOURCE_ALIAS "hooks/ThemeController.qml"
        QT_QML_SINGLETON_TYPE TRUE
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ModelSelector.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ModelSelector.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/FileAttachmentChip.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/FileAttachmentChip.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ImageAttachment.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ImageAttachment.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/TerminalPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/TerminalPage.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/ExportDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/ExportDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ToolCallCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ToolCallCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AgentProgressPanel.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AgentProgressPanel.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/ToolConfirmationDialog.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/ToolConfirmationDialog.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/PlanCard.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/PlanCard.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/components/AudioPlayer.qml
    PROPERTIES QT_RESOURCE_ALIAS "components/AudioPlayer.qml"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/frontend/pages/ArtifactViewerPage.qml
    PROPERTIES QT_RESOURCE_ALIAS "pages/ArtifactViewerPage.qml"
)

set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/00-introduction.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/00-introduction.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/01-quick-start.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/01-quick-start.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/02-providers.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/02-providers.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/03-conversations.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/03-conversations.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/04-multi-agent-teams.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/04-multi-agent-teams.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/05-projects.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/05-projects.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/06-canvas-and-tasks.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/06-canvas-and-tasks.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/07-skills.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/07-skills.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/08-remote-android.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/08-remote-android.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/09-activity-timeline.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/09-activity-timeline.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/10-troubleshooting.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/10-troubleshooting.md"
)
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/Documentation/User/11-faq.md
    PROPERTIES QT_RESOURCE_ALIAS "user-docs/11-faq.md"
)

set(_verzeta_qml_no_cachegen_arg "")
if(WIN32)
    set(_verzeta_qml_no_cachegen_arg NO_CACHEGEN)
endif()

qt_add_qml_module(verzeta-studio
    URI org.verzeta.studio
    VERSION 1.0
    QML_FILES
        ${CMAKE_SOURCE_DIR}/frontend/pages/MainWindow.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/HomePage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/LeftSidebar.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/ChatPanel.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/RightSettingsPanel.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/CanvasPanel.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/SettingsPanel.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/TextProvidersPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/ImageProvidersPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/SearchProvidersPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/RagpProvidersPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/EmbeddingsProvidersPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/ToolsPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/AgentsPage.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/AppButton.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AppRadioButton.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CallOverlay.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AppOverlayDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FrostedGlass.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AppTextField.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AppTextArea.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FeaturePill.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FolderSettingsDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/NewGroupChatDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/MembershipEditor.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AddChatMemberDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ModelOverrideEditor.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/SearchableModelComboBox.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ChatInputBar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CompactionGauge.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ModelSelectorBar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/SidebarConvItem.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FolderSectionHeader.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/SidebarSectionHeader.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/SidebarConvContextMenu.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CanvasActionBar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CanvasConsole.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FirstRunWizard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AboutDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/IconNavBar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CanvasEditor.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProviderCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CustomServerCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CustomServerSetupSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ImageProviderSetupSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/PollCard.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/PlansOverlay.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatActivityOverlay.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatDiagnosticsTab.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/HeartbeatAuditTab.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/ActivityTimeline.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/HelpOverlay.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomOverlay.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateBanner.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectStatusPill.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectFilterPill.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AgentAvatar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomTexture.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectQuickStartSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectRoomKickoffSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateDetailSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ProjectTemplateLibrarySheet.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/RemoteAccessDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/RevokedDevicesSheet.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/VoiceCallsDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ExecutionPermissionsOverlay.qml

        ${CMAKE_SOURCE_DIR}/frontend/pages/SkillsPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/SkillReviewDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/PreferredSkillsOverlay.qml

        ${CMAKE_SOURCE_DIR}/frontend/components/MessageBubble.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AssistantThinkingDisclosure.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/CodeBlock.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/MarkdownText.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/TypingIndicator.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/StopRetryBar.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/TokenCounter.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/StatChip.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ToolActivityIndicator.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ModelSelector.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/FileAttachmentChip.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ImageAttachment.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ToolCallCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AgentProgressPanel.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/ToolConfirmationDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/PlanCard.qml
        ${CMAKE_SOURCE_DIR}/frontend/components/AudioPlayer.qml

        ${CMAKE_SOURCE_DIR}/frontend/pages/ExportDialog.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/TerminalPage.qml
        ${CMAKE_SOURCE_DIR}/frontend/pages/ArtifactViewerPage.qml

        ${CMAKE_SOURCE_DIR}/frontend/hooks/ThemeController.qml
    RESOURCES
        ${CMAKE_SOURCE_DIR}/Documentation/User/00-introduction.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/01-quick-start.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/02-providers.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/03-conversations.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/04-multi-agent-teams.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/05-projects.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/06-canvas-and-tasks.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/07-skills.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/08-remote-android.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/09-activity-timeline.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/10-troubleshooting.md
        ${CMAKE_SOURCE_DIR}/Documentation/User/11-faq.md
    RESOURCE_PREFIX /qt/qml
    NO_PLUGIN_OPTIONAL
    ${_verzeta_qml_no_cachegen_arg}
)

target_link_libraries(verzeta-studio PRIVATE
    verzeta-studio-backend
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Qml
    KF6::Kirigami
)


install(TARGETS verzeta-studio
    RUNTIME DESTINATION bin
)

if(SQLITE_VEC_LOADABLE)
    add_custom_command(TARGET verzeta-studio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SQLITE_VEC_LOADABLE}" "$<TARGET_FILE_DIR:verzeta-studio>"
        COMMENT "Staging sqlite-vec loadable next to verzeta-studio")
    install(FILES "${SQLITE_VEC_LOADABLE}" DESTINATION bin)
endif()

add_executable(verzeta-remote
    remote/server/main.cpp
    remote/server/wire-server.cpp
    remote/server/wire-session.cpp
    remote/server/wire-host-client.cpp
    remote/server/wire-auth.cpp
    remote/server/wire-db-reader.cpp
)

target_include_directories(verzeta-remote PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(verzeta-remote PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
)

install(TARGETS verzeta-remote
    RUNTIME DESTINATION bin
)

if(VERZETA_ENABLE_LLAMACPP)
    add_executable(verzeta-inference
        inference/main.cpp
        inference/infer-protocol.cpp
        inference/embed-slot.cpp
        inference/model-loader.cpp
        inference/ragp-slot.cpp
    )
    target_include_directories(verzeta-inference PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(verzeta-inference PRIVATE
        Qt6::Core
    )
    if(WIN32)
        set_target_properties(verzeta-inference PROPERTIES WIN32_EXECUTABLE ON)
    endif()
    if(VERZETA_HAS_LLAMA)
        target_link_libraries(verzeta-inference PRIVATE llama)
        target_compile_definitions(verzeta-inference PRIVATE
            VERZETA_INFER_HAS_LLAMA=1)
    endif()
    install(TARGETS verzeta-inference
        RUNTIME DESTINATION bin
    )
endif()

add_executable(verzeta-splash
    splash/main.cpp
)

set_source_files_properties("${CMAKE_SOURCE_DIR}/frontend/splash/Splash.qml"
    PROPERTIES QT_RESOURCE_ALIAS "Splash.qml")
set_source_files_properties("${CMAKE_SOURCE_DIR}/resources/icons/verzeta-studio.png"
    PROPERTIES QT_RESOURCE_ALIAS "verzeta-studio.png")

qt_add_resources(verzeta-splash "verzeta_splash_assets"
    PREFIX "/splash"
    FILES
        "${CMAKE_SOURCE_DIR}/frontend/splash/Splash.qml"
        "${CMAKE_SOURCE_DIR}/resources/icons/verzeta-studio.png"
)

target_link_libraries(verzeta-splash PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)

if(WIN32)
    set_target_properties(verzeta-splash PROPERTIES WIN32_EXECUTABLE ON)
endif()

install(TARGETS verzeta-splash
    RUNTIME DESTINATION bin
)
