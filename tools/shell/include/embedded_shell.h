#pragma once

#include "linenoise.h"
#include "main/lbug.h"
#include "printer/printer.h"

namespace lbug {
namespace main {

const int defaultMaxRows = 20;

struct ShellConfig {
    const char* path_to_history = "";
    uint64_t maxRowSize = defaultMaxRows;
    uint32_t maxPrintWidth = 0;
    std::unique_ptr<Printer> printer = std::make_unique<BoxPrinter>();
    bool stats = true;
    // Agent mode (compact Markdown, JSON errors, capped results). See agent_mode.h.
    bool agentMode = false;
    // Name of the agent-identifying env var ("" if forced via flag/env).
    std::string agentEnv;
    // True when agent mode turned on by automatic detection (env + pipe).
    bool agentAutoDetected = false;
    // True when --agent/--no-agent was passed (suppresses the failure hint).
    bool agentFlagGiven = false;
    // True when stdout is a terminal.
    bool stdoutIsTTY = false;
};

/**
 * Embedded shell simulate a session that directly connects to the system.
 */
class EmbeddedShell {

public:
    EmbeddedShell(std::shared_ptr<Database> database, std::shared_ptr<Connection> conn,
        ShellConfig& shellConfig);

    void run();

    std::vector<std::unique_ptr<QueryResult>> processInput(std::string input);

    void printErrorMessage(std::string input, QueryResult& queryResult);

    static void interruptHandler(int signal);

private:
    int processShellCommands(std::string lineStr);

    static void printHelp();

    void printExecutionResult(QueryResult& queryResult) const;

    void printTruncatedExecutionResult(QueryResult& queryResult) const;

    std::string printJsonExecutionResult(QueryResult& queryResult) const;

    std::string printHtmlExecutionResult(QueryResult& queryResult) const;

    std::string printLatexExecutionResult(QueryResult& queryResult) const;

    std::string printLineExecutionResult(QueryResult& queryResult) const;

    void updateTableNames();

    void updateFunctionAndTypeNames();

    void setMaxRows(const std::string& maxRowsString);

    void setMaxWidth(const std::string& maxWidthString);

    void setMode(const std::string& modeString);

    void setStats(const std::string& statsString);

    void setLinenoiseMode(int mode);

    void setHighlighting(const std::string& highlightingString);

    void setErrors(const std::string& errorsString);

    void setComplete(const std::string& completeString);

    void printSchema();

    void checkConfidentialStatement(const std::string& query, QueryResult* queryResult,
        std::string& input);

private:
    std::shared_ptr<Database> database;
    std::shared_ptr<Connection> conn;
    const char* path_to_history;
    uint64_t maxRowSize;
    uint32_t maxPrintWidth;
    std::unique_ptr<Printer> printer;
    bool stats;
    uint64_t catalogVersion;
    // Agent mode state.
    bool agentMode;
    std::string agentEnv;
    bool agentAutoDetected;
    bool agentFlagGiven;
    bool stdoutIsTTY;
    bool modeExplicitlySet = false;
    // "auto" (follow agent mode), "compact" or "box".
    std::string explainFormat = "auto";
    int64_t agentMaxRows = 1000;
    int64_t agentMaxBytes = 10000;
    int64_t agentMaxCellWidth = 500;
    bool hadError = false;

    void printAgentExecutionResult(QueryResult& queryResult) const;

    void printCompactExplain(QueryResult& queryResult) const;

    void printAgentStatus() const;

    void printAgentHelp() const;

    void printAgentTables();

    void setAgentCap(const std::string& name, const std::string& value);

    void setExplainFormat(const std::string& format);
};

} // namespace main
} // namespace lbug
