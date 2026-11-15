#include "embedded_shell.h"

#include "binder/visitor/confidential_statement_analyzer.h"

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <iomanip>
#include <regex>
#include <sstream>

#include "agent_mode.h"
#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/catalog_entry.h"
#include "catalog/catalog_entry/index_catalog_entry.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "catalog/catalog_entry/rel_group_catalog_entry.h"
#include "catalog/catalog_entry/scalar_macro_catalog_entry.h"
#include "catalog/catalog_entry/sequence_catalog_entry.h"
#include "catalog/schema_graph.h"
#include "common/exception/parser.h"
#include "common/types/value/nested.h"
#include "common/types/value/value.h"
#include "extension/extension_manager.h"
#include "keywords.h"
#include "parser/parser.h"
#include "printer/json_printer.h"
#include "printer/printer_factory.h"
#include "transaction/transaction.h"
#include "transaction/transaction_context.h"
#include "utf8proc.h"
#include "utf8proc_wrapper.h"

using namespace lbug::common;
using namespace lbug::utf8proc;

namespace lbug {
namespace main {

#ifdef _WIN32
#ifndef STDIN_FILENO
#define STDIN_FILENO (_fileno(stdin))
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO (_fileno(stdout))
#endif
#endif

// prompt for user input
const char* PROMPT = "lbug> ";
const char* ALTPROMPT = "..> ";
const char* CONPROMPT = "\u00B7 ";
const char* SCONPROMPT = "\u2023 ";

// build-in shell command
struct ShellCommand {
    const char* HELP = ":help";
    const char* CLEAR = ":clear";
    const char* QUIT = ":quit";
    const char* MAX_ROWS = ":max_rows";
    const char* MAX_WIDTH = ":max_width";
    const char* MODE = ":mode";
    const char* STATS = ":stats";
    const char* MULTI = ":multiline";
    const char* SINGLE = ":singleline";
    const char* HIGHLIGHT = ":highlight";
    const char* ERRORS = ":render_errors";
    const char* COMPLETE = ":render_completion";
    const char* SCHEMA = ":schema";
    const char* MAXROWS = ":maxrows";
    const char* MAXBYTES = ":maxbytes";
    const char* MAXCELLWIDTH = ":maxcellwidth";
    const char* TABLES = ":tables";
    const char* SHOW = ":show";
    const char* EXPLAIN_FMT = ":explain";
    const std::array<const char*, 19> commandList = {HELP, CLEAR, QUIT, MAX_ROWS, MAX_WIDTH, MODE,
        STATS, MULTI, SINGLE, HIGHLIGHT, ERRORS, COMPLETE, SCHEMA, MAXROWS, MAXBYTES, MAXCELLWIDTH,
        TABLES, SHOW, EXPLAIN_FMT};
} shellCommand;

const char* TAB = "    ";

const std::array<const char*, _keywordListLength> keywordList = _keywordList;

const char* keywordColorPrefix = "\033[32m\033[1m";
const char* keywordResetPostfix = "\033[39m\033[22m";

// NOLINTNEXTLINE(cert-err58-cpp): OK to have a global regex, even if the constructor allocates.
const std::regex specialChars{R"([-[\]{}()*+?.,\^$|#\s])"};

std::vector<std::string> nodeTableNames;
std::vector<std::string> relTableNames;
std::unordered_map<std::string, std::vector<std::string>> tableColumnNames;

std::vector<std::string> functionAndTypeNames;
std::vector<std::string> tableFunctionNames;
std::vector<std::string> aggregateFunctionNames;
std::vector<std::string> scalarFunctionNames;

bool continueLine = false;
std::string currLine;
std::string historyLine;

static Connection* globalConnection;

bool printInterrupted = false;

#ifndef _WIN32
struct termios orig_termios;
bool noEcho = false;
#else
DWORD oldOutputCP;
#endif

void EmbeddedShell::updateTableNames() {
    nodeTableNames.clear();
    relTableNames.clear();
    auto clientContext = conn->getClientContext();
    bool transactionStarted = false;

    if (transaction::Transaction::Get(*clientContext) == NULL) {
        transaction::TransactionContext::Get(*clientContext)
            ->beginReadTransaction(); // start transaction to get current table names
        transactionStarted = true;
    }
    for (auto& tableEntry : database->catalog->getTableEntries(
             transaction::Transaction::Get(*clientContext), false /*useInternal*/)) {
        if (tableEntry->getType() == catalog::CatalogEntryType::NODE_TABLE_ENTRY) {
            nodeTableNames.push_back(tableEntry->getName());
        } else if (tableEntry->getType() == catalog::CatalogEntryType::REL_GROUP_ENTRY) {
            relTableNames.push_back(tableEntry->getName());
        } else {
            continue;
        }
        std::vector<std::string> columnNames;
        for (auto& column : tableEntry->getProperties()) {
            columnNames.push_back(column.getName());
        }
        tableColumnNames[tableEntry->getName()] = columnNames;
    }
    if (transactionStarted) {
        transaction::TransactionContext::Get(*clientContext)->commit();
    }
}

void EmbeddedShell::updateFunctionAndTypeNames() {
    functionAndTypeNames.clear();
    auto typeNames = LogicalTypeUtils::getAllValidLogicTypeIDs();
    for (auto& type : typeNames) {
        functionAndTypeNames.push_back(LogicalTypeUtils::toString(type));
    }
    for (auto& entry : database->catalog->getFunctionEntries(&transaction::DUMMY_TRANSACTION)) {
        switch (entry->getType()) {
        case catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY:
            aggregateFunctionNames.push_back(entry->getName());
            break;
        case catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY:
            scalarFunctionNames.push_back(entry->getName());
            break;
        case catalog::CatalogEntryType::TABLE_FUNCTION_ENTRY:
        case catalog::CatalogEntryType::STANDALONE_TABLE_FUNCTION_ENTRY:
            tableFunctionNames.push_back(entry->getName());
            break;
        default:
            functionAndTypeNames.push_back(entry->getName());
            break;
        }
    }
}

void addTableCompletion(const std::string& buf, const std::string& tableName,
    linenoiseCompletions* lc) {
    std::string prefix, suffix;
    auto prefixPos = buf.rfind(':') + 1;
    prefix = buf.substr(0, prefixPos);
    suffix = buf.substr(prefixPos);
    if (suffix == tableName.substr(0, suffix.length())) {
        linenoiseAddCompletion(lc, (prefix + tableName).c_str());
    }
}

bool isInsideCommentOrQuote(const std::string& buffer) {
    bool insideSingleLineComment = false;
    bool insideMultiLineComment = false;
    bool insideDoubleQuote = false;
    bool insideSingleQuote = false;

    for (size_t i = 0; i < buffer.size(); ++i) {
        if (insideSingleLineComment) {
            if (buffer[i] == '\n') {
                insideSingleLineComment = false;
            }
        } else if (insideMultiLineComment) {
            if (buffer[i] == '*' && i + 1 < buffer.size() && buffer[i + 1] == '/') {
                insideMultiLineComment = false;
                ++i;
            }
        } else if (insideDoubleQuote) {
            if (buffer[i] == '"' && (i == 0 || buffer[i - 1] != '\\')) {
                insideDoubleQuote = false;
            }
        } else if (insideSingleQuote) {
            if (buffer[i] == '\'' && (i == 0 || buffer[i - 1] != '\\')) {
                insideSingleQuote = false;
            }
        } else {
            if (buffer[i] == '/' && i + 1 < buffer.size()) {
                if (buffer[i + 1] == '/') {
                    insideSingleLineComment = true;
                    ++i;
                } else if (buffer[i + 1] == '*') {
                    insideMultiLineComment = true;
                    ++i;
                }
            } else if (buffer[i] == '"') {
                insideDoubleQuote = true;
            } else if (buffer[i] == '\'') {
                insideSingleQuote = true;
            }
        }
    }

    return insideSingleLineComment || insideMultiLineComment || insideDoubleQuote ||
           insideSingleQuote;
}

void findTableVariableNames(const std::string buf, std::regex tableRegex,
    std::vector<std::string>& tempTableNames, std::vector<std::string>& foundTableNames) {
    auto matches_begin = std::sregex_iterator(buf.begin(), buf.end(), tableRegex);
    auto matches_end = std::sregex_iterator();

    for (std::sregex_iterator i = matches_begin; i != matches_end; ++i) {
        std::smatch match = *i;
        tempTableNames.push_back(match[1].str());
        foundTableNames.push_back(match[2].str());
    }
}

void keywordCompletion(std::string buf, std::string prefix, std::string keyword,
    linenoiseCompletions* lc) {
    std::string bufEscaped = regex_replace(buf, specialChars, R"(\$&)");
    if (regex_search(keyword, std::regex("^" + bufEscaped, std::regex_constants::icase))) {
        std::string transformedKeyword = keyword;
        for (size_t i = 0; i < buf.size() && i < keyword.size(); ++i) {
            if (islower(buf[i])) {
                transformedKeyword[i] = tolower(keyword[i]);
            } else if (isupper(buf[i])) {
                transformedKeyword[i] = toupper(keyword[i]);
            }
        }
        // make the rest of the keyword the same case as the last character of buf
        auto caseTransform = islower(buf.back()) ? ::tolower : ::toupper;
        std::transform(keyword.begin() + buf.size(), keyword.end(),
            transformedKeyword.begin() + buf.size(), caseTransform);

        linenoiseAddCompletion(lc, (prefix + transformedKeyword).c_str());
    }
}

void completion(const char* buffer, linenoiseCompletions* lc) {
    std::string buf = std::string(buffer);

    // Command completion.
    if (buf[0] == ':') {
        for (auto& command : shellCommand.commandList) {
            if (regex_search(command, std::regex("^" + buf))) {
                linenoiseAddCompletion(lc, command);
            }
        }
        return;
    }

    // Skip completion if inside a comment or quote
    if (isInsideCommentOrQuote(buf)) {
        return;
    }

    // RETURN *; completion for MATCH and CALL queries.
    // Trigger when buffer ends with ')' or ') ' after a MATCH pattern or CALL function.
    if (regex_search(buf, std::regex(R"(\)\s*$)"))) {
        // Check for MATCH pattern: MATCH(var:Table) or MATCH (var:Table)
        bool isMatchQuery =
            regex_search(buf, std::regex(R"(^\s*MATCH\s*\()", std::regex_constants::icase));
        // Check for CALL function: CALL func_name(...) or CALL func_name (...)
        bool isCallFunction =
            regex_search(buf, std::regex(R"(^\s*CALL\s+\w+\s*\()", std::regex_constants::icase));
        if (isMatchQuery || isCallFunction) {
            std::string suffix = buf.back() == ')' ? " RETURN *;" : "RETURN *;";
            linenoiseAddCompletion(lc, (buf + suffix).c_str());
            return;
        }
    }

    // Node table name completion. Match patterns that include an open bracket `(` with no closing
    // bracket `)`, and a colon `:` sometime after the open bracket.
    if (regex_search(buf, std::regex("^[^]*\\([^\\)]*:[^\\)]*$"))) {
        for (auto& node : nodeTableNames) {
            addTableCompletion(buf, node, lc);
        }
        return;
    }

    // Rel table name completion. Matches patterns that
    // include an open square bracket `[` with no closing
    // bracket `]` and a colon `:` sometime after the open bracket.
    if (regex_search(buf, std::regex("^[^]*\\[[^\\]]*:[^\\]]*$"))) {
        for (auto& rel : relTableNames) {
            addTableCompletion(buf, rel, lc);
        }
        return;
    }

    std::vector<std::string> tempTableNames;
    std::vector<std::string> foundTableNames;
    for (auto& node : nodeTableNames) {
        std::regex nodeTableRegex("\\(([^:\\(]+):(" + node + ")\\)");
        findTableVariableNames(buf, nodeTableRegex, tempTableNames, foundTableNames);
    }
    for (auto& rel : relTableNames) {
        std::regex relTableRegex("\\[([^:\\[]+):(" + rel + ")\\]");
        findTableVariableNames(buf, relTableRegex, tempTableNames, foundTableNames);
    }

    // Keyword completion.
    std::string prefix;
    auto lastKeywordPos = buf.rfind(' ') + 1;
    if (lastKeywordPos != std::string::npos) {
        prefix = buf.substr(0, lastKeywordPos);
        buf = buf.substr(lastKeywordPos);
    }
    if (buf.empty()) {
        return;
    }

    for (size_t i = 0; i < tempTableNames.size(); ++i) {
        if (regex_search(buf, std::regex("^" + tempTableNames[i] + R"(\.[^.]*)"))) {
            auto it = tableColumnNames.find(foundTableNames[i]);
            if (it != tableColumnNames.end()) {
                auto columnNames = it->second;
                for (auto& columnName : columnNames) {
                    if (regex_search(columnName,
                            std::regex("^" + buf.substr(tempTableNames[i].size() + 1)))) {
                        linenoiseAddCompletion(lc,
                            (prefix + tempTableNames[i] + "." + columnName).c_str());
                    }
                }
            }
            return;
        }
    }

    if (regex_search(prefix, std::regex("^\\s*CALL\\s*$", std::regex_constants::icase))) {
        for (auto& function : tableFunctionNames) {
            keywordCompletion(buf, prefix, function, lc);
        }
        return;
    }

    for (std::string keyword : keywordList) {
        keywordCompletion(buf, prefix, keyword, lc);
    }

    for (std::string function : aggregateFunctionNames) {
        keywordCompletion(buf, prefix, function, lc);
    }

    for (std::string function : scalarFunctionNames) {
        keywordCompletion(buf, prefix, function, lc);
    }

    for (std::string function : functionAndTypeNames) {
        keywordCompletion(buf, prefix, function, lc);
    }
}

uint64_t damerauLevenshteinDistance(const std::string& s1, const std::string& s2) {
    const uint64_t m = s1.size(), n = s2.size();
    std::vector<std::vector<uint64_t>> dp(m + 1, std::vector<uint64_t>(n + 1, 0));
    for (uint64_t i = 0; i <= m; i++) {
        dp[i][0] = i;
    }
    for (uint64_t j = 0; j <= n; j++) {
        dp[0][j] = j;
    }
    for (uint64_t i = 1; i <= m; i++) {
        for (uint64_t j = 1; j <= n; j++) {
            if (s1[i - 1] == s2[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1];
                if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
                    dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2]);
                }
            } else {
                dp[i][j] = 1 + std::min({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1]});
                if (i > 1 && j > 1 && s1[i - 1] == s2[j - 2] && s1[i - 2] == s2[j - 1]) {
                    dp[i][j] = std::min(dp[i][j], dp[i - 2][j - 2] + 1);
                }
            }
        }
    }
    return dp[m][n];
}

std::string findClosestCommand(std::string lineStr) {
    std::string closestCommand = "";
    uint64_t editDistance = INT_MAX;
    for (auto& command : shellCommand.commandList) {
        auto distance = damerauLevenshteinDistance(command, lineStr);
        if (distance < editDistance) {
            editDistance = distance;
            closestCommand = command;
        }
    }
    return closestCommand;
}

static std::string normalizeShellCommand(const std::string& raw) {
    // Accept both ':' and '.' prefixes and match case-insensitively.
    std::string cmd = raw;
    if (!cmd.empty() && (cmd[0] == ':' || cmd[0] == '.')) {
        cmd = cmd.substr(1);
    }
    std::transform(cmd.begin(), cmd.end(), cmd.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return cmd;
}

int EmbeddedShell::processShellCommands(std::string lineStr) {
    std::istringstream iss(lineStr);
    std::string rawCommand, arg;
    iss >> rawCommand >> arg;
    std::string command = normalizeShellCommand(rawCommand);
    if (command == "help") {
        if (arg == "agent") {
            printAgentHelp();
        } else {
            printHelp();
        }
    } else if (command == "clear") {
        linenoiseClearScreen();
    } else if (command == "quit" || command == "exit") {
        return -1;
    } else if (command == "max_rows") {
        setMaxRows(arg);
    } else if (command == "max_width") {
        setMaxWidth(arg);
    } else if (command == "mode") {
        setMode(arg);
    } else if (command == "stats") {
        if (arg.empty()) {
            printAgentStatus();
        } else {
            setStats(arg);
        }
    } else if (command == "multiline") {
        setLinenoiseMode(1);
    } else if (command == "singleline") {
        setLinenoiseMode(0);
    } else if (command == "highlight") {
        setHighlighting(arg);
    } else if (command == "render_errors" || command == "errors") {
        setErrors(arg);
    } else if (command == "render_completion" || command == "complete") {
        setComplete(arg);
    } else if (command == "schema") {
        printSchema();
    } else if (command == "maxrows" || command == "maxbytes" || command == "maxcellwidth") {
        setAgentCap(command, arg);
    } else if (command == "tables") {
        printAgentTables();
    } else if (command == "show") {
        printAgentStatus();
    } else if (command == "explain") {
        setExplainFormat(arg);
    } else {
        hadError = true;
        if (agentMode) {
            fprintf(stderr, "%s\n",
                agent::shellErrorJson("Unknown command: \"" + lineStr +
                                      "\". Enter "
                                      "\".help\" for help")
                    .c_str());
        } else {
            printf("Error: Unknown command: \"%s\". Enter \":help\" for help\n", lineStr.c_str());
            printf("Did you mean: \"%s\"?\n", findClosestCommand(lineStr).c_str());
        }
    }
    return 0;
}

EmbeddedShell::EmbeddedShell(std::shared_ptr<Database> database, std::shared_ptr<Connection> conn,
    ShellConfig& shellConfig) {
    path_to_history = shellConfig.path_to_history;
    linenoiseHistoryLoad(path_to_history);
    linenoiseSetCompletionCallback(completion);
    this->database = database;
    this->conn = conn;
    globalConnection = conn.get();
    maxRowSize = shellConfig.maxRowSize;
    maxPrintWidth = shellConfig.maxPrintWidth;
    printer = std::move(shellConfig.printer);
    stats = shellConfig.stats;
    agentMode = shellConfig.agentMode;
    agentEnv = shellConfig.agentEnv;
    agentAutoDetected = shellConfig.agentAutoDetected;
    agentFlagGiven = shellConfig.agentFlagGiven;
    stdoutIsTTY = shellConfig.stdoutIsTTY;
    catalogVersion = database->catalog->getVersion();
    updateTableNames();
    updateFunctionAndTypeNames();
    auto sigResult = std::signal(SIGINT, interruptHandler);
    if (sigResult == SIG_ERR) {
        throw std::runtime_error("Error: Failed to set signal handler");
    }
}

std::string decodeEscapeSequences(const std::string& input) {
    std::regex unicodeRegex(R"(\\u([0-9A-Fa-f]{4})|\\U([0-9A-Fa-f]{8}))");
    std::string result = input;
    std::smatch match;
    while (std::regex_search(result, match, unicodeRegex)) {
        std::string codepointStr;
        if (match[1].matched) {
            codepointStr = match[1].str();
        } else if (match[2].matched) {
            codepointStr = match[2].str();
        }
        uint32_t codepoint = static_cast<uint32_t>(std::stoull(codepointStr, nullptr, 16));
        if (codepoint > 0x10FFFF) {
            throw std::runtime_error("Invalid Unicode codepoint");
        }
        if (codepoint == 0) {
            throw std::runtime_error("Null character not allowed");
        }
        // Check for surrogate pairs
        if (0xD800 <= codepoint && codepoint <= 0xDBFF) {
            // High surrogate, look for the next low surrogate
            std::smatch nextMatch;
            std::string remainingString = result.substr(match.position() + match.length());
            if (std::regex_search(remainingString, nextMatch, unicodeRegex)) {
                std::string nextCodepointStr = nextMatch[1].str();
                int nextCodepoint = std::stoi(nextCodepointStr, nullptr, 16);
                if (0xDC00 <= nextCodepoint && nextCodepoint <= 0xDFFF) {
                    // Valid surrogate pair
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (nextCodepoint - 0xDC00);
                    result.replace(match.position() + match.length(), nextMatch.length(), "");
                } else {
                    throw std::runtime_error("Invalid surrogate pair");
                }
            } else {
                throw std::runtime_error("Unmatched high surrogate");
            }
        }

        // Convert codepoint to UTF-8
        char utf8Char[5] = {0}; // UTF-8 characters can be up to 4 bytes + null terminator
        int size = 0;
        if (!Utf8Proc::codepointToUtf8(codepoint, size, utf8Char)) {
            throw std::runtime_error("Failed to convert codepoint to UTF-8");
        }

        // Replace the escape sequence with the actual UTF-8 character
        result.replace(match.position(), match.length(), std::string(utf8Char, size));
    }
    return result;
}

void EmbeddedShell::checkConfidentialStatement(const std::string& query, QueryResult* queryResult,
    std::string& input) {
    if (queryResult->isSuccess() && !database->isReadOnly() &&
        queryResult->getQuerySummary()->getStatementType() == StatementType::STANDALONE_CALL) {
        auto clientContext = conn->getClientContext();
        auto parsedQueries = clientContext->parseQuery(query);
        auto transactionContext = transaction::TransactionContext::Get(*clientContext);
        for (auto& parsedQuery : parsedQueries) {
            transactionContext->beginWriteTransaction();
            auto binder = binder::Binder(clientContext);
            auto boundQuery = binder.bind(*parsedQuery);
            auto boundStatementVisitor = binder::ConfidentialStatementAnalyzer{};
            boundStatementVisitor.visit(*boundQuery);
            if (boundStatementVisitor.isConfidential()) {
                input = "";
            }
            transactionContext->commit();
        }
    }
}

std::vector<std::unique_ptr<QueryResult>> EmbeddedShell::processInput(std::string input) {
    std::stringstream ss;
    std::vector<std::unique_ptr<QueryResult>> queryResults;
    historyLine = "";
    // Append rest of multiline query to current input line
    if (continueLine) {
        input = std::move(currLine) + std::move(input);
        currLine = "";
        continueLine = false;
    }
    input = input.erase(input.find_last_not_of(" \t\n\r\f\v") + 1);
    // Decode escape sequences
    std::string unicodeInput;
    try {
        unicodeInput = decodeEscapeSequences(input);
    } catch (std::exception& e) {
        printf("Error: %s\n", e.what());
        historyLine = input;
        return queryResults;
    }
    // Normalize trailing semicolons
    if (!unicodeInput.empty() && unicodeInput.back() == ';') {
        // trim trailing ;
        while (!unicodeInput.empty() && unicodeInput.back() == ';') {
            unicodeInput.pop_back();
        }
        if (unicodeInput.empty()) {
            return queryResults;
        }
        unicodeInput += ';';
    }
    // process shell commands (':' commands and DuckDB-style '.' commands)
    if (!continueLine && !unicodeInput.empty() &&
        (unicodeInput[0] == ':' || unicodeInput[0] == '.')) {
        processShellCommands(unicodeInput);
        // process queries
    } else if (!unicodeInput.empty() && cypherComplete((char*)unicodeInput.c_str())) {
        ss.clear();
        ss.str(unicodeInput);
        auto result = conn->query(unicodeInput);
        auto curr = result.get();
        checkConfidentialStatement(unicodeInput, curr, input);
        while (true) {
            queryResults.push_back(std::move(result));
            if (!curr->hasNextQueryResult()) {
                break;
            }
            result = curr->moveNextResult();
            curr = result.get();
            checkConfidentialStatement(unicodeInput, curr, input);
        }
        // set up multiline query if current query doesn't end with a semicolon
    } else if (!input.empty() && input[0] != ':') {
        continueLine = true;
        currLine += input + "\n";
    }
    if (catalogVersion != database->catalog->getVersion()) {
        updateTableNames();
        catalogVersion = database->catalog->getVersion();
    }
    historyLine = input;
    return queryResults;
}

void EmbeddedShell::printErrorMessage(std::string input, QueryResult& queryResult) {
    hadError = true;
    input = input.erase(input.find_last_not_of(" \t\n\r\f\v") + 1);
    std::string errMsg = queryResult.getErrorMessage();
    if (agentMode) {
        std::string stripped;
        auto type = agent::exceptionTypeFromMessage(errMsg, stripped);
        fprintf(stderr, "%s\n", agent::formatErrorJson(type, stripped).c_str());
        return;
    }
    printf("Error: %s\n", errMsg.c_str());
    if (errMsg.find(ParserException::ERROR_PREFIX) == 0) {
        std::string trimmedinput = input;
        trimmedinput.erase(0, trimmedinput.find_first_not_of(" \t\n\r\f\v"));
        if (trimmedinput.find_first_of(" \t\n\r\f\v") == std::string::npos) {
            printf("\"%s\" is not a valid Cypher query. Did you mean to issue a "
                   "CLI command, e.g., \"%s\"?\n",
                input.c_str(), findClosestCommand(input).c_str());
        }
    }
}

void EmbeddedShell::run() {
    char* line = nullptr;
    const char ctrl_c = '\3';
    int numCtrlC = 0;
    // `true` when a multiline query is incomplete. See `EmbeddedShell::processInput`.
    continueLine = false;
    currLine = "";

#ifndef _WIN32
    termios raw{};
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
            errno = ENOTTY;
            return;
        }
        raw = orig_termios;
        raw.c_lflag &= ~ECHO;

        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
            errno = ENOTTY;
            return;
        }
        noEcho = true;
    }
#else
    oldOutputCP = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
#endif

    while (true) {
        line = linenoise(continueLine ? ALTPROMPT : PROMPT, CONPROMPT, SCONPROMPT);

        if (line == nullptr && !continueLine) {
            // EOF is reached and there is no input left to process. Exit loop.
            break;
        }

        std::string lineStr;
        if (line == nullptr) {
            // EOF is reached, but there is an incomplete query (`continueLine` == true).
            // Mark as complete with a semicolon and attempt to process the query.
            lineStr = ";";
        } else {
            // Not EOF. We take the input as is.
            lineStr = std::string(line);
        }
        lineStr = lineStr.erase(lineStr.find_last_not_of(" \t\n\r\f\v") + 1);
        if (!lineStr.empty() && lineStr[0] == ctrl_c) {
            if (!continueLine && lineStr[1] == '\0') {
                numCtrlC++;
                if (numCtrlC >= 2) {
                    free(line);
                    break;
                }
            }
            currLine = "";
            continueLine = false;
            free(line);
            continue;
        }
        numCtrlC = 0;
        auto queryResults = processInput(lineStr);
        if (queryResults.empty()) {
            std::istringstream iss(lineStr);
            std::string command;
            iss >> command;
            if (command == shellCommand.QUIT) {
                free(line);
                break;
            }
        }
        for (auto& queryResult : queryResults) {
            if (queryResult->isSuccess()) {
                printInterrupted = false;
                printExecutionResult(*queryResult);
            } else {
                printErrorMessage(lineStr, *queryResult);
            }
        }

        if (!continueLine && !historyLine.empty()) {
            linenoiseHistoryAdd(historyLine.c_str());
        }
        linenoiseHistorySave(path_to_history);
        free(line);
    }

    if (!agentMode && agentEnv.empty() && !agentFlagGiven && !stdoutIsTTY && hadError) {
        fprintf(stderr, "hint: --agent renders errors as JSON and results compactly for AI coding "
                        "agents (lbug --help lists all options; --no_agent disables this hint)\n");
    }
#ifndef _WIN32
    /* Don't even check the return value as it's too late. */
    if (noEcho && tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios) != -1) {
        noEcho = false;
    }
#else
    SetConsoleOutputCP(oldOutputCP);
#endif
}

void EmbeddedShell::interruptHandler(int /*signal*/) {
    globalConnection->interrupt();
    printInterrupted = true;
}

void EmbeddedShell::setLinenoiseMode(int mode) {
    linenoiseSetMultiLine(mode, path_to_history);
    if (mode == 0) {
        printf("Single line mode enabled\n");
    } else {
        printf("Multi line mode enabled\n");
    }
}

void EmbeddedShell::setMaxRows(const std::string& maxRowsString) {
    uint64_t parsedMaxRows = 0;
    try {
        parsedMaxRows = stoull(maxRowsString);
    } catch (std::exception& e) {
        printf("Cannot parse '%s' as number of rows. Expect integer.\n", maxRowsString.c_str());
        return;
    }
    maxRowSize = parsedMaxRows == 0 ? defaultMaxRows : parsedMaxRows;
    printf("maxRows set as %" PRIu64 "\n", maxRowSize);
}

void EmbeddedShell::setMaxWidth(const std::string& maxWidthString) {
    uint32_t parsedMaxWidth = 0;
    try {
        parsedMaxWidth = stoul(maxWidthString);
    } catch (std::exception& e) {
        printf("Cannot parse '%s' as number of characters. Expect integer.\n",
            maxWidthString.c_str());
        return;
    }
    maxPrintWidth = parsedMaxWidth;
    printf("maxWidth set as %d\n", parsedMaxWidth);
}

void printModeInfo() {
    printf("Available output modes:\n");
    printf("%sbox (default):%sTables using unicode box-drawing characters\n", TAB, TAB);
    printf("%scolumn:%sOutput in columns\n", TAB, TAB);
    printf("%scsv:%sComma-separated values\n", TAB, TAB);
    printf("%shtml:%sHTML table\n", TAB, TAB);
    printf("%sjson:%sResults in a JSON array\n", TAB, TAB);
    printf("%sjsonlines:%sResults in a NDJSON format\n", TAB, TAB);
    printf("%slatex:%sLaTeX tabular environment code\n", TAB, TAB);
    printf("%sline:%sOne value per line\n", TAB, TAB);
    printf("%slist:%sValues delimited by \"|\"\n", TAB, TAB);
    printf("%smarkdown:%sMarkdown table\n", TAB, TAB);
    printf("%stable:%sTables using ASCII characters\n", TAB, TAB);
    printf("%stsv:%sTab-separated values\n", TAB, TAB);
    printf("%strash:%sNo output\n", TAB, TAB);
}

void EmbeddedShell::setMode(const std::string& modeString) {
    modeExplicitlySet = true;
    if (modeString.empty()) {
        printModeInfo();
        return;
    }
    auto modePrinter = PrinterFactory::getPrinter(PrinterTypeUtils::fromString(modeString));
    if (modePrinter == nullptr) {
        printf("Cannot parse '%s' as output mode.\n\n", modeString.c_str());
        printModeInfo();
        return;
    }
    stats = modePrinter->defaultPrintStats();
    printer = std::move(modePrinter);
    printf("mode set as %s\n", modeString.c_str());
}

void EmbeddedShell::setStats(const std::string& statsString) {
    std::string statsStringLower = statsString;
    std::transform(statsStringLower.begin(), statsStringLower.end(), statsStringLower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (statsStringLower == "on") {
        stats = true;
    } else if (statsStringLower == "off") {
        stats = false;
    } else {
        printf("Cannot parse '%s' to toggle stats. Expect 'on' or 'off'.\n", statsString.c_str());
        return;
    }
    printf("stats set as %s\n", stats ? "on" : "off");
}

void EmbeddedShell::setHighlighting(const std::string& highlightString) {
    std::string highlightStringLower = highlightString;
    std::transform(highlightStringLower.begin(), highlightStringLower.end(),
        highlightStringLower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (highlightStringLower == "on") {
        linenoiseSetHighlighting(1);
        printf("enabled syntax highlighting\n");
    } else if (highlightStringLower == "off") {
        linenoiseSetHighlighting(0);
        printf("disabled syntax highlighting\n");
    } else {
        printf("Cannot parse '%s' to toggle highlighting. Expect 'on' or 'off'.\n",
            highlightString.c_str());
        return;
    }
}

void EmbeddedShell::setErrors(const std::string& errorsString) {
    std::string errorsStringLower = errorsString;
    std::transform(errorsStringLower.begin(), errorsStringLower.end(), errorsStringLower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (errorsStringLower == "on") {
        linenoiseSetErrors(1);
        printf("enabled error highlighting\n");
    } else if (errorsStringLower == "off") {
        linenoiseSetErrors(0);
        printf("disabled error highlighting\n");
    } else {
        printf("Cannot parse '%s' to toggle error highlighting. Expect 'on' or 'off'.\n",
            errorsStringLower.c_str());
        return;
    }
}

void EmbeddedShell::setComplete(const std::string& completeString) {
    std::string completeStringLower = completeString;
    std::transform(completeStringLower.begin(), completeStringLower.end(),
        completeStringLower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (completeStringLower == "on") {
        linenoiseSetCompletion(1);
        printf("enabled completion highlighting\n");
    } else if (completeStringLower == "off") {
        linenoiseSetCompletion(0);
        printf("disabled completion highlighting\n");
    } else {
        printf("Cannot parse '%s' to toggle completion highlighting. Expect 'on' or 'off'.\n",
            completeStringLower.c_str());
    }
}

void EmbeddedShell::printHelp() {
    printf("%s%s %sget command list\n", TAB, shellCommand.HELP, TAB);
    printf("%s%s %sclear shell\n", TAB, shellCommand.CLEAR, TAB);
    printf("%s%s %sexit from shell\n", TAB, shellCommand.QUIT, TAB);
    printf("%s%s [max_rows] %sset maximum number of rows for display (default: 20)\n", TAB,
        shellCommand.MAX_ROWS, TAB);
    printf("%s%s [max_width] %sset maximum width in characters for display\n", TAB,
        shellCommand.MAX_WIDTH, TAB);
    printf("%s%s [mode] %sset output mode (default: box)\n", TAB, shellCommand.MODE, TAB);
    printf("%s%s [on|off] %stoggle query stats on or off\n", TAB, shellCommand.STATS, TAB);
    printf("%s%s %sset multiline mode (default)\n", TAB, shellCommand.MULTI, TAB);
    printf("%s%s %sset singleline mode\n", TAB, shellCommand.SINGLE, TAB);
    printf("%s%s [on|off] %stoggle syntax highlighting on or off\n", TAB, shellCommand.HIGHLIGHT,
        TAB);
    printf("%s%s [on|off] %stoggle error highlighting on or off\n", TAB, shellCommand.ERRORS, TAB);
    printf("%s%s [on|off] %stoggle completion highlighting on or off\n", TAB, shellCommand.COMPLETE,
        TAB);
    printf("%s%s %sprint database schema\n", TAB, shellCommand.SCHEMA, TAB);
    printf("%s%s [N] %sset agent-mode row cap (0/-1 = unlimited)\n", TAB, ":maxrows", TAB);
    printf("%s%s [N] %sset agent-mode byte cap (0/-1 = unlimited)\n", TAB, ":maxbytes", TAB);
    printf("%s%s [N] %sset agent-mode max cell width\n", TAB, ":maxcellwidth", TAB);
    printf("%s%s %slist tables with row counts and columns\n", TAB, ":tables", TAB);
    printf("%s%s %sshow agent status\n", TAB, ":show", TAB);
    printf("%s%s [compact|box|auto] %sset EXPLAIN format\n", TAB, ":explain", TAB);
    printf("%s%s %sagent mode help\n", TAB, ":help agent", TAB);
    printf("\n");
    printf("%sNote: you can change and see several system configurations, such as num-threads, \n",
        TAB);
    printf("%s%s  timeout, and progress_bar using Cypher CALL statements.\n", TAB, TAB);
    printf("%s%s  e.g. CALL THREADS=5; or CALL current_setting('threads') return *;\n", TAB, TAB);
    const char* url = "https://docs.ladybugdb.com/cypher/configuration";
    printf("%s%s  See: \x1B]8;;%s\x1B\\%s\x1B]8;;\x1B\\\n", TAB, TAB, url, url);
}

std::string EmbeddedShell::printJsonExecutionResult(QueryResult& queryResult) const {
    auto& jsonPrinter = printer->constCast<JsonPrinter>();
    return jsonPrinter.print(queryResult, *storage::MemoryManager::Get(*conn->getClientContext()));
}

std::string escapeHtmlString(const std::string& str) {
    std::ostringstream escaped;
    for (char c : str) {
        switch (c) {
        case '&':
            escaped << "&amp;";
            break;
        case '\"':
            escaped << "&quot;";
            break;
        case '\'':
            escaped << "&apos;";
            break;
        case '<':
            escaped << "&lt;";
            break;
        case '>':
            escaped << "&gt;";
            break;
        default:
            escaped << c;
        }
    }
    return escaped.str();
}

std::string EmbeddedShell::printHtmlExecutionResult(QueryResult& queryResult) const {
    auto colNames = queryResult.getColumnNames();
    auto& pHtmlPrinter = printer->constCast<HTMLPrinter>();
    std::string printString = pHtmlPrinter.TableOpen;
    printString += "\n";
    printString += pHtmlPrinter.RowOpen;
    printString += "\n";
    for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
        printString += pHtmlPrinter.HeaderOpen;
        printString += escapeHtmlString(colNames[i]);
        printString += pHtmlPrinter.HeaderClose;
    }
    printString += "\n";
    printString += pHtmlPrinter.RowClose;
    printString += "\n";
    while (queryResult.hasNext()) {
        if (printInterrupted) {
            return std::string();
        }
        auto tuple = queryResult.getNext();
        printString += pHtmlPrinter.RowOpen;
        printString += "\n";
        for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
            printString += pHtmlPrinter.CellOpen;
            printString += escapeHtmlString(tuple->getValue(i)->toString());
            printString += pHtmlPrinter.CellClose;
        }
        printString += pHtmlPrinter.RowClose;
        printString += "\n";
    }
    printString += pHtmlPrinter.TableClose;
    printString += "\n";
    return printString;
}

std::string escapeLatexString(const std::string& str) {
    std::ostringstream escaped;
    for (char c : str) {
        switch (c) {
        case '&':
            escaped << "\\&";
            break;
        case '%':
            escaped << "\\%";
            break;
        case '$':
            escaped << "\\$";
            break;
        case '#':
            escaped << "\\#";
            break;
        case '_':
            escaped << "\\_";
            break;
        case '{':
            escaped << "\\{";
            break;
        case '}':
            escaped << "\\}";
            break;
        case '~':
            escaped << "\\textasciitilde{}";
            break;
        case '^':
            escaped << "\\textasciicircum{}";
            break;
        case '\\':
            escaped << "\\textbackslash{}";
            break;
        case '<':
            escaped << "\\textless{}";
            break;
        case '>':
            escaped << "\\textgreater{}";
            break;
        default:
            escaped << c;
        }
    }
    return escaped.str();
}

std::string EmbeddedShell::printLatexExecutionResult(QueryResult& queryResult) const {
    auto colNames = queryResult.getColumnNames();
    auto& latexPrinter = printer->constCast<LatexPrinter>();
    std::string printString = latexPrinter.TableOpen;
    printString += latexPrinter.AlignOpen;
    for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
        printString += latexPrinter.ColumnAlign;
    }
    printString += latexPrinter.AlignClose;
    printString += "\n";
    printString += latexPrinter.Line;
    printString += "\n";
    for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
        printString += escapeLatexString(colNames[i]);
        if (i != queryResult.getNumColumns() - 1) {
            printString += latexPrinter.TupleDelimiter;
        }
    }
    printString += latexPrinter.EndLine;
    printString += "\n";
    printString += latexPrinter.Line;
    printString += "\n";
    while (queryResult.hasNext()) {
        if (printInterrupted) {
            return std::string();
        }
        auto tuple = queryResult.getNext();
        for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
            printString += escapeLatexString(tuple->getValue(i)->toString());
            if (i != queryResult.getNumColumns() - 1) {
                printString += latexPrinter.TupleDelimiter;
            }
        }
        printString += latexPrinter.EndLine;
        printString += "\n";
    }
    printString += latexPrinter.Line;
    printString += "\n";
    printString += latexPrinter.TableClose;
    printString += "\n";
    return printString;
}

std::string EmbeddedShell::printLineExecutionResult(QueryResult& queryResult) const {
    auto colNames = queryResult.getColumnNames();
    std::string printString = "";
    while (queryResult.hasNext()) {
        if (printInterrupted) {
            return std::string();
        }
        auto tuple = queryResult.getNext();
        for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
            printString += colNames[i];
            printString += printer->TupleDelimiter;
            printString += tuple->getValue(i)->toString();
            printString += "\n";
        }
        printString += "\n";
    }
    return printString;
}

std::string escapeCsvString(const std::string& field, const std::string& delimiter) {
    bool needsQuoting =
        field.find_first_of("\"" + delimiter + "\n") != std::string::npos || field.empty();
    if (!needsQuoting) {
        return field;
    }

    std::ostringstream quotedField;
    quotedField << '"';
    for (char c : field) {
        if (c == '"') {
            quotedField << "\"\"";
        } else {
            quotedField << c;
        }
    }
    quotedField << '"';
    return quotedField.str();
}

// TODO(Ziyi): Move the printing logic under each printer and each printer should expose a unified
// `print` interface.
void EmbeddedShell::printExecutionResult(QueryResult& queryResult) const {
    auto querySummary = queryResult.getQuerySummary();
    if (querySummary->isExplain()) {
        if (explainFormat == "compact" || (agentMode && explainFormat == "auto")) {
            printCompactExplain(queryResult);
            return;
        }
        printf("%s", queryResult.getNext()->toString().c_str());
        return;
    }
    if (agentMode && !modeExplicitlySet && PrinterTypeUtils::isTableType(printer->printType)) {
        printAgentExecutionResult(queryResult);
        return;
    }
    if (PrinterTypeUtils::isTableType(printer->printType)) {
        printTruncatedExecutionResult(queryResult);
        return;
    }
    std::string printString = "";
    if (printer->printType == PrinterType::JSON || printer->printType == PrinterType::JSONLINES) {
        printString = printJsonExecutionResult(queryResult);
    } else if (printer->printType == PrinterType::HTML) {
        printString = printHtmlExecutionResult(queryResult);
    } else if (printer->printType == PrinterType::LATEX) {
        printString = printLatexExecutionResult(queryResult);
    } else if (printer->printType == PrinterType::LINE) {
        printString = printLineExecutionResult(queryResult);
    } else if (printer->printType != PrinterType::TRASH) {
        for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
            printString +=
                escapeCsvString(queryResult.getColumnNames()[i], printer->TupleDelimiter);
            if (i != queryResult.getNumColumns() - 1) {
                printString += printer->TupleDelimiter;
            }
        }
        printString += "\n";
        while (queryResult.hasNext()) {
            if (printInterrupted) {
                return;
            }
            auto tuple = queryResult.getNext();
            for (auto i = 0u; i < queryResult.getNumColumns(); i++) {
                std::string field = tuple->getValue(i)->toString();
                printString += escapeCsvString(field, printer->TupleDelimiter);
                if (i != queryResult.getNumColumns() - 1) {
                    printString += printer->TupleDelimiter;
                }
            }
            printString += "\n";
        }
    }
    if (!printInterrupted) {
        printf("%s", printString.c_str());
    }
    if (stats && !printInterrupted) {
        if (queryResult.getNumTuples() == 1) {
            printf("(1 tuple)\n");
        } else {
            printf("(%" PRIu64 " tuples)\n", queryResult.getNumTuples());
        }
        if (queryResult.getNumColumns() == 1) {
            printf("(1 column)\n");
        } else {
            printf("(%" PRIu64 " columns)\n", static_cast<uint64_t>(queryResult.getNumColumns()));
        }
        printf("Time: %.2fms (compiling), %.2fms (executing)\n", querySummary->getCompilingTime(),
            querySummary->getExecutionTime());
    }
}

// ---------------------------------------------------------------------------
// Render a MAP(STRING, INT64) value as a compact single-line visual.
// Falls back to default toString() for other types.
// ---------------------------------------------------------------------------
static std::string renderMapCell(const common::Value& val) {
    if (val.isNull() || val.getDataType().getLogicalTypeID() != common::LogicalTypeID::MAP) {
        return val.toString();
    }

    const auto& mapType = val.getDataType();
    const auto& keyType = common::MapType::getKeyType(mapType);
    const auto& valType = common::MapType::getValueType(mapType);
    if (keyType.getLogicalTypeID() != common::LogicalTypeID::STRING ||
        valType.getLogicalTypeID() != common::LogicalTypeID::INT64) {
        return val.toString();
    }

    auto numBins = common::NestedVal::getChildrenSize(&val);
    if (numBins == 0) {
        return "(empty)";
    }

    // Collect counts and find max
    std::vector<int64_t> counts(numBins);
    std::vector<std::string> labels(numBins);
    int64_t maxCount = 1;
    for (uint32_t i = 0; i < numBins; ++i) {
        auto* entry = common::NestedVal::getChildVal(&val, i);
        auto* keyVal = common::NestedVal::getChildVal(entry, 0);
        auto* cntVal = common::NestedVal::getChildVal(entry, 1);
        labels[i] = keyVal->toString();
        counts[i] = cntVal->getValue<int64_t>();
        if (counts[i] > maxCount)
            maxCount = counts[i];
    }

    // Unicode blocks U+2581..U+2588 as raw UTF-8
    static const char* BLOCKS = "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84"
                                "\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88";

    // Build sparkline
    std::string spark;
    for (uint32_t i = 0; i < numBins; ++i) {
        int level = static_cast<int>(
            std::round(static_cast<double>(counts[i]) * 7.0 / static_cast<double>(maxCount)));
        spark.append(BLOCKS + level * 3, 3);
    }

    // Build compact summary:  [1.00-1.00:2, 2.00-2.00:2, 5.00-10.00:7]
    std::string summary = "  [";
    for (uint32_t i = 0; i < numBins; ++i) {
        if (i > 0)
            summary += ", ";
        summary += labels[i] + ":" + std::to_string(counts[i]);
    }
    summary += "]";

    return spark + summary;
}

void EmbeddedShell::printTruncatedExecutionResult(QueryResult& queryResult) const {
    auto& baseTablePrinter = printer->constCast<BaseTablePrinter>();
    auto querySummary = queryResult.getQuerySummary();
    uint64_t numTuples = queryResult.getNumTuples();
    std::vector<uint32_t> colsWidth(queryResult.getNumColumns(), 2);
    // calculate the width of each column name/type
    for (auto i = 0u; i < colsWidth.size(); i++) {
        colsWidth[i] = std::max(queryResult.getColumnNames()[i].length(),
                           queryResult.getColumnDataTypes()[i].toString().length()) +
                       2;
    }
    uint64_t rowCount = 0;
    // calculate the width of each tuple value
    while (queryResult.hasNext()) {
        if (numTuples > maxRowSize && rowCount >= (maxRowSize / 2) + (maxRowSize % 2 != 0) &&
            rowCount < numTuples - maxRowSize / 2) {
            auto tuple = queryResult.getNext();
            rowCount++;
            continue;
        }
        auto tuple = queryResult.getNext();
        for (auto i = 0u; i < colsWidth.size(); i++) {
            if (tuple->getValue(i)->isNull()) {
                continue;
            }
            // Use renderMapCell for MAP columns to get visual rendering width
            auto colType = queryResult.getColumnDataTypes()[i].getLogicalTypeID();
            std::string tupleString;
            if (colType == common::LogicalTypeID::MAP) {
                tupleString = renderMapCell(*tuple->getValue(i));
                // MAP cells tend to be wider than other types; ensure a usable
                // minimum width so subsequent rows of the same column get
                // reasonable space.
                colsWidth[i] = std::max(colsWidth[i], (uint32_t)60);
            } else {
                tupleString = tuple->getValue(i)->toString();
            }
            uint32_t fieldLen = 0;
            uint32_t chrIter = 0;
            while (chrIter < tupleString.length()) {
                fieldLen += Utf8Proc::renderWidth(tupleString.c_str(), chrIter);
                chrIter =
                    utf8proc_next_grapheme(tupleString.c_str(), tupleString.length(), chrIter);
            }
            // An extra 2 spaces are added for an extra space on either
            // side of the std::string.
            colsWidth[i] = std::max(colsWidth[i], fieldLen + 2);
        }
        rowCount++;
    }

    // calculate the maximum width of the table
    uint32_t sumGoal = BaseTablePrinter::MIN_TRUNCATED_WIDTH;
    if (colsWidth.size() == 1) {
        uint32_t minDisplayWidth =
            BaseTablePrinter::MIN_TRUNCATED_WIDTH + BaseTablePrinter::SMALL_TABLE_SEPERATOR_LENGTH;
        if (maxPrintWidth > minDisplayWidth) {
            sumGoal = maxPrintWidth - 2;
        } else {
            sumGoal =
                std::max((uint32_t)(getColumns(STDIN_FILENO, STDOUT_FILENO) - colsWidth.size() - 1),
                    minDisplayWidth);
        }
    } else if (colsWidth.size() > 1) {
        uint32_t minDisplayWidth = BaseTablePrinter::SMALL_TABLE_SEPERATOR_LENGTH +
                                   BaseTablePrinter::MIN_TRUNCATED_WIDTH * 2;
        if (maxPrintWidth > minDisplayWidth) {
            sumGoal = maxPrintWidth - colsWidth.size() - 1;
        } else {
            // make sure there is space for the first and last column
            sumGoal =
                std::max((uint32_t)(getColumns(STDIN_FILENO, STDOUT_FILENO) - colsWidth.size() - 1),
                    minDisplayWidth);
        }
    } else if (maxPrintWidth > BaseTablePrinter::MIN_TRUNCATED_WIDTH) {
        sumGoal = maxPrintWidth;
    }
    uint32_t sum = 0;
    std::vector<uint32_t> maxValueIndex;
    uint32_t secondHighestValue = 0;
    for (auto i = 0u; i < colsWidth.size(); i++) {
        if (maxValueIndex.empty() || colsWidth[i] == colsWidth[maxValueIndex[0]]) {
            maxValueIndex.push_back(i);
        } else if (colsWidth[i] > colsWidth[maxValueIndex[0]]) {
            secondHighestValue = colsWidth[maxValueIndex[0]];
            maxValueIndex.clear();
            maxValueIndex.push_back(i);
        } else if (colsWidth[i] > secondHighestValue) {
            secondHighestValue = colsWidth[i];
        }
        sum += colsWidth[i];
    }

    // truncate columns
    while (sum > sumGoal) {
        uint32_t truncationValue = ((sum - sumGoal) / maxValueIndex.size()) +
                                   ((sum - sumGoal) % maxValueIndex.size() != 0);
        uint32_t newValue = 0;
        if (truncationValue < colsWidth[maxValueIndex[0]]) {
            newValue = colsWidth[maxValueIndex[0]] - truncationValue;
        }
        uint32_t oldValue = colsWidth[maxValueIndex[0]];
        if (secondHighestValue < BaseTablePrinter::MIN_TRUNCATED_WIDTH + 2 &&
            newValue < BaseTablePrinter::MIN_TRUNCATED_WIDTH + 2) {
            newValue = BaseTablePrinter::MIN_TRUNCATED_WIDTH + 2;
        } else {
            uint32_t sumDifference = sum - ((oldValue - secondHighestValue) * maxValueIndex.size());
            if (sumDifference > sumGoal) {
                newValue = secondHighestValue;
            }
        }
        for (auto i = 0u; i < maxValueIndex.size(); i++) {
            colsWidth[maxValueIndex[i]] = newValue;
        }
        sum -= (oldValue - newValue) * maxValueIndex.size();
        if (newValue == BaseTablePrinter::MIN_TRUNCATED_WIDTH + 2) {
            break;
        }

        maxValueIndex.clear();
        secondHighestValue = 0;
        for (auto i = 0u; i < colsWidth.size(); i++) {
            if (maxValueIndex.empty() || colsWidth[i] == colsWidth[maxValueIndex[0]]) {
                maxValueIndex.push_back(i);
            } else if (colsWidth[i] > colsWidth[maxValueIndex[0]]) {
                secondHighestValue = colsWidth[maxValueIndex[0]];
                maxValueIndex.clear();
                maxValueIndex.push_back(i);
            } else if (colsWidth[i] > secondHighestValue) {
                secondHighestValue = colsWidth[i];
            }
        }
    }

    // remove columns
    uint32_t k = 0;
    uint32_t j = colsWidth.size() - 1;
    bool colTruncated = false;
    uint64_t colsPrinted = 0;
    uint32_t lineSeparatorLen = 1u;
    for (auto i = 0u; i < colsWidth.size(); i++) {
        if (k <= j) {
            if ((lineSeparatorLen + colsWidth[k] < sumGoal + colsWidth.size() - 5) ||
                (lineSeparatorLen + colsWidth[k] < sumGoal + colsWidth.size() + 1 &&
                    (colTruncated || k == j))) {
                lineSeparatorLen += colsWidth[k] + 1;
                k++;
                colsPrinted++;
            } else if (!colTruncated) {
                lineSeparatorLen += 6;
                colTruncated = true;
            }
        }
        if (j >= k) {
            if ((lineSeparatorLen + colsWidth[j] < sumGoal + colsWidth.size() - 5) ||
                (lineSeparatorLen + colsWidth[j] < sumGoal + colsWidth.size() + 1 &&
                    (colTruncated || j == k))) {
                lineSeparatorLen += colsWidth[j] + 1;
                j--;
                colsPrinted++;
            } else if (!colTruncated) {
                lineSeparatorLen += 6;
                colTruncated = true;
            }
        }
    }

    if (queryResult.getNumColumns() != 0 && baseTablePrinter.TopLine) {
        std::string printString;
        printString += baseTablePrinter.DownAndRight;
        for (auto i = 0u; i < k; i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.DownAndHorizontal;
            }
        }
        if (j >= k) {
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.DownAndHorizontal;
        }
        for (auto i = j + 1; i < colsWidth.size(); i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.DownAndHorizontal;
            }
        }
        printString += baseTablePrinter.DownAndLeft;
        printf("%s\n", printString.c_str());
    }

    if (queryResult.getNumColumns() != 0 && !queryResult.getColumnNames()[0].empty()) {
        std::string printString;
        printString += baseTablePrinter.Vertical;
        for (auto i = 0u; i < k; i++) {
            std::string columnName = queryResult.getColumnNames()[i];
            uint32_t chrIter = 0;
            uint32_t fieldLen = 0;
            while (chrIter < columnName.length()) {
                fieldLen += Utf8Proc::renderWidth(columnName.c_str(), chrIter);
                chrIter = utf8proc_next_grapheme(columnName.c_str(), columnName.length(), chrIter);
            }
            uint32_t truncationLen = fieldLen;
            if (truncationLen > colsWidth[i] - 2) {
                while (truncationLen > colsWidth[i] - 5) {
                    chrIter = Utf8Proc::previousGraphemeCluster(columnName.c_str(),
                        columnName.length(), chrIter);
                    truncationLen -= Utf8Proc::renderWidth(columnName.c_str(), chrIter);
                }
                columnName = columnName.substr(0, chrIter) + baseTablePrinter.Truncation;
                truncationLen += strlen(baseTablePrinter.Truncation);
            }
            printString += " ";
            printString += columnName;
            printString += std::string(colsWidth[i] - truncationLen - 1, ' ');
            if (i != k - 1) {
                printString += baseTablePrinter.Vertical;
            }
        }
        if (j >= k) {
            printString += baseTablePrinter.Vertical;
            printString += " ";
            printString += baseTablePrinter.Truncation;
            printString += " ";
        }
        for (auto i = j + 1; i < colsWidth.size(); i++) {
            printString += baseTablePrinter.Vertical;
            std::string columnName = queryResult.getColumnNames()[i];
            uint32_t chrIter = 0;
            uint32_t fieldLen = 0;
            while (chrIter < columnName.length()) {
                fieldLen += Utf8Proc::renderWidth(columnName.c_str(), chrIter);
                chrIter = utf8proc_next_grapheme(columnName.c_str(), columnName.length(), chrIter);
            }
            uint32_t truncationLen = fieldLen;
            if (truncationLen > colsWidth[i] - 2) {
                while (truncationLen > colsWidth[i] - 5) {
                    chrIter = Utf8Proc::previousGraphemeCluster(columnName.c_str(),
                        columnName.length(), chrIter);
                    truncationLen -= Utf8Proc::renderWidth(columnName.c_str(), chrIter);
                }
                columnName = columnName.substr(0, chrIter) + baseTablePrinter.Truncation;
                truncationLen += strlen(baseTablePrinter.Truncation);
            }
            printString += " ";
            printString += columnName;
            printString += std::string(colsWidth[i] - truncationLen - 1, ' ');
        }
        printString += baseTablePrinter.Vertical;
        if (baseTablePrinter.Types) {
            printString += "\n";
            printString += baseTablePrinter.Vertical;
            for (auto i = 0u; i < k; i++) {
                std::string columnType = queryResult.getColumnDataTypes()[i].toString();
                if (columnType.length() > colsWidth[i] - 2) {
                    columnType =
                        columnType.substr(0, colsWidth[i] - 5) + baseTablePrinter.Truncation;
                }
                printString += " ";
                printString += columnType;
                printString += std::string(colsWidth[i] - columnType.length() - 1, ' ');
                if (i != k - 1) {
                    printString += baseTablePrinter.Vertical;
                }
            }
            if (j >= k) {
                printString += baseTablePrinter.Vertical;
                printString += "     ";
            }
            for (auto i = j + 1; i < colsWidth.size(); i++) {
                std::string columnType = queryResult.getColumnDataTypes()[i].toString();
                if (columnType.length() > colsWidth[i] - 2) {
                    columnType =
                        columnType.substr(0, colsWidth[i] - 5) + baseTablePrinter.Truncation;
                }
                printString += baseTablePrinter.Vertical;
                printString += " ";
                printString += columnType;
                printString += std::string(colsWidth[i] - columnType.length() - 1, ' ');
            }
            printString += baseTablePrinter.Vertical;
        }
        printf("%s\n", printString.c_str());
    }

    if (queryResult.getNumColumns() != 0) {
        std::string printString;
        printString += baseTablePrinter.VerticalAndRight;
        for (auto i = 0u; i < k; i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.VerticalAndHorizontal;
            }
        }
        if (j >= k) {
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.VerticalAndHorizontal;
        }
        for (auto i = j + 1; i < colsWidth.size(); i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.VerticalAndHorizontal;
            }
        }
        printString += baseTablePrinter.VerticalAndLeft;
        printf("%s\n", printString.c_str());
    }

    queryResult.resetIterator();
    rowCount = 0;
    bool rowTruncated = false;
    while (queryResult.hasNext()) {
        if (printInterrupted) {
            return;
        }
        if (numTuples > maxRowSize && rowCount >= (maxRowSize / 2) + (maxRowSize % 2 != 0) &&
            rowCount < numTuples - maxRowSize / 2) {
            auto tuple = queryResult.getNext();
            if (!rowTruncated) {
                rowTruncated = true;
                for (auto i = 0u; i < 3u; i++) {
                    std::string printString;
                    printString += baseTablePrinter.Vertical;
                    for (auto l = 0u; l < k; l++) {
                        uint32_t spacesToPrint = (colsWidth[l] / 2);
                        printString += std::string(spacesToPrint - 1, ' ');
                        printString += baseTablePrinter.MiddleDot;
                        if (colsWidth[l] % 2 == 1) {
                            printString += " ";
                        }
                        printString += std::string(spacesToPrint, ' ');
                        if (l != k - 1) {
                            printString += baseTablePrinter.Vertical;
                        }
                    }
                    if (j >= k) {
                        printString += baseTablePrinter.Vertical;
                        printString += "  ";
                        printString += baseTablePrinter.MiddleDot;
                        printString += "  ";
                    }
                    for (auto l = j + 1; l < colsWidth.size(); l++) {
                        uint32_t spacesToPrint = (colsWidth[l] / 2);
                        printString += baseTablePrinter.Vertical;
                        printString += std::string(spacesToPrint - 1, ' ');
                        printString += baseTablePrinter.MiddleDot;
                        if (colsWidth[l] % 2 == 1) {
                            printString += " ";
                        }
                        printString += std::string(spacesToPrint, ' ');
                    }
                    printString += baseTablePrinter.Vertical;
                    printf("%s\n", printString.c_str());
                }
            }
            rowCount++;
            continue;
        }
        auto tuple = queryResult.getNext();
        // Build formatted column strings, using renderMapCell for MAP columns
        std::vector<std::string> colResults(colsWidth.size());
        for (auto i = 0u; i < colsWidth.size(); i++) {
            std::string cellStr;
            if (tuple->getValue(i)->isNull()) {
                cellStr = std::string(colsWidth[i], ' ');
            } else {
                auto colType = queryResult.getColumnDataTypes()[i].getLogicalTypeID();
                if (colType == common::LogicalTypeID::MAP) {
                    cellStr = renderMapCell(*tuple->getValue(i));
                } else {
                    cellStr = tuple->getValue(i)->toString();
                }
                // Truncate/pad to column width
                uint32_t fieldLen = 0;
                uint32_t chrIter = 0;
                uint32_t cutoff = 0;
                uint32_t cutoffLen = 0;
                while (chrIter < cellStr.length()) {
                    auto w = Utf8Proc::renderWidth(cellStr.c_str(), chrIter);
                    if (fieldLen + w > colsWidth[i] - 2 && cutoff == 0) {
                        cutoff = chrIter;
                        cutoffLen = fieldLen;
                    }
                    fieldLen += w;
                    chrIter = utf8proc_next_grapheme(cellStr.c_str(), cellStr.length(), chrIter);
                }
                if (fieldLen > colsWidth[i] - 2) {
                    cellStr = cellStr.substr(0, cutoff) + "...";
                    fieldLen = cutoffLen + 3;
                }
                cellStr = " " + std::move(cellStr) + " ";
                fieldLen += 2;
                if (fieldLen < colsWidth[i]) {
                    cellStr += std::string(colsWidth[i] - fieldLen, ' ');
                }
            }
            colResults[i] = cellStr;
        }

        std::string printString;
        printString += baseTablePrinter.Vertical;
        for (auto i = 0u; i < k; i++) {
            printString += colResults[i];
            if (i != k - 1) {
                printString += baseTablePrinter.Vertical;
            }
        }
        if (j >= k) {
            printString += baseTablePrinter.Vertical;
            printString += " ";
            printString += baseTablePrinter.Truncation;
            printString += " ";
        }
        for (auto i = j + 1; i < colResults.size(); i++) {
            printString += baseTablePrinter.Vertical;
            printString += colResults[i];
        }
        printString += baseTablePrinter.Vertical;
        printf("%s\n", printString.c_str());
        rowCount++;
    }

    if (queryResult.getNumColumns() != 0 && baseTablePrinter.BottomLine) {
        std::string printString;
        printString += baseTablePrinter.UpAndRight;
        for (auto i = 0u; i < k; i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.UpAndHorizontal;
            }
        }
        if (j >= k) {
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.Horizontal;
            printString += baseTablePrinter.UpAndHorizontal;
        }
        for (auto i = j + 1; i < colsWidth.size(); i++) {
            for (auto l = 0u; l < colsWidth[i]; l++) {
                printString += baseTablePrinter.Horizontal;
            }
            if (i != colsWidth.size() - 1) {
                printString += baseTablePrinter.UpAndHorizontal;
            }
        }
        printString += baseTablePrinter.UpAndLeft;
        printf("%s\n", printString.c_str());
    }

    // print query result (numFlatTuples & tuples)
    if (stats) {
        if (numTuples == 1) {
            printf("(1 tuple)\n");
        } else {
            printf("(%" PRIu64 " tuples", numTuples);
            if (rowTruncated) {
                printf(", %" PRIu64 " shown", maxRowSize);
            }
            printf(")\n");
        }
        if (colsWidth.size() == 1) {
            printf("(1 column)\n");
        } else {
            printf("(%" PRIu64 " columns", (uint64_t)colsWidth.size());
            if (colTruncated) {
                printf(", %" PRIu64 " shown", colsPrinted);
            }
            printf(")\n");
        }
        printf("Time: %.2fms (compiling), %.2fms (executing)\n", querySummary->getCompilingTime(),
            querySummary->getExecutionTime());
    }
}

void EmbeddedShell::setAgentCap(const std::string& name, const std::string& value) {
    int64_t* target = nullptr;
    if (name == "maxrows") {
        target = &agentMaxRows;
    } else if (name == "maxbytes") {
        target = &agentMaxBytes;
    } else if (name == "maxcellwidth") {
        target = &agentMaxCellWidth;
    }
    if (value.empty()) {
        printf("agent %s is %lld (0/-1 = unlimited)\n", name.c_str(),
            static_cast<long long>(*target));
        return;
    }
    try {
        int64_t parsed = std::stoll(value);
        if (parsed < -1) {
            printf("Cannot parse '%s' as %s. Expect a non-negative integer "
                   "(0/-1 = unlimited).\n",
                value.c_str(), name.c_str());
            return;
        }
        *target = parsed;
    } catch (std::exception&) {
        printf("Cannot parse '%s' as %s. Expect a non-negative integer "
               "(0/-1 = unlimited).\n",
            value.c_str(), name.c_str());
        return;
    }
    printf("agent %s set as %lld\n", name.c_str(), static_cast<long long>(*target));
}

void EmbeddedShell::setExplainFormat(const std::string& format) {
    std::string lower = format;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    if (lower.empty()) {
        printf("explain format is %s\n", explainFormat.c_str());
        return;
    }
    if (lower == "compact") {
        explainFormat = "compact";
    } else if (lower == "box" || lower == "default") {
        explainFormat = "box";
    } else if (lower == "auto") {
        explainFormat = "auto";
    } else {
        printf("Cannot parse '%s' as explain format. Expect 'compact', 'box' or 'auto'.\n",
            format.c_str());
        return;
    }
    printf("explain format set as %s\n", explainFormat.c_str());
}

void EmbeddedShell::printAgentExecutionResult(QueryResult& queryResult) const {
    auto colNames = queryResult.getColumnNames();
    auto colTypes = queryResult.getColumnDataTypes();
    const auto numCols = queryResult.getNumColumns();
    const uint64_t totalRows = queryResult.getNumTuples();
    // Header carries the column types: | name:TYPE | n:INT64 |, then |---|.
    std::string header = "|";
    std::string separator = "|";
    for (auto i = 0u; i < numCols; i++) {
        header +=
            " " + agent::markdownEscapeCell(colNames[i]) + ":" + colTypes[i].toString() + " |";
        separator += "---|";
    }
    printf("%s\n%s\n", header.c_str(), separator.c_str());
    if (numCols == 0) {
        return;
    }
    const bool rowsLimited = agentMaxRows > 0 && totalRows > (uint64_t)agentMaxRows;
    // Single pass: keep the first 20 and a rolling window of the last 20 rows,
    // plus a byte count and an order-independent hash of the full result.
    struct SampledRow {
        std::string text;
    };
    std::vector<SampledRow> first;
    std::vector<SampledRow> last;
    first.reserve(20);
    last.reserve(20);
    uint64_t renderedBytes = 0;
    uint64_t totalHash = 0;
    queryResult.resetIterator();
    while (queryResult.hasNext()) {
        if (printInterrupted) {
            return;
        }
        auto tuple = queryResult.getNext();
        std::string rowText = "|";
        uint64_t rowHash = 1469598103934665603ULL;
        for (auto i = 0u; i < numCols; i++) {
            std::string cell;
            if (tuple->getValue(i)->isNull()) {
                // Tagged so that NULL hashes differently from the string 'NULL'.
                rowHash = agent::fnv1a("\x00NULL", 5, rowHash);
                cell = "NULL";
            } else {
                std::string raw = tuple->getValue(i)->toString();
                rowHash = agent::fnv1a("\x01", 1, rowHash);
                rowHash = agent::fnv1a(raw.data(), raw.size(), rowHash);
                if (agentMaxCellWidth > 0 && agent::utf8Length(raw) > (size_t)agentMaxCellWidth) {
                    size_t omitted = 0;
                    // NB: keep the truncation on its own statement: it writes `omitted`,
                    // which is only read afterwards. Folding both into one `+` chain leaves
                    // the order unspecified (operands of `operator+` are indeterminately
                    // sequenced), so some toolchains report `+0 chars`.
                    std::string kept = agent::utf8Truncate(raw, (size_t)agentMaxCellWidth, omitted);
                    raw = kept + "…(+" + std::to_string(omitted) + " chars)";
                }
                cell = agent::markdownEscapeCell(raw);
            }
            rowText += " " + cell + " |";
        }
        rowHash = agent::fnv1a(reinterpret_cast<const char*>(&numCols), sizeof(numCols), rowHash);
        totalHash ^= rowHash; // order-independent combination
        renderedBytes += rowText.size() + 1;
        SampledRow row{rowText};
        if (first.size() < 20) {
            first.push_back(row);
        }
        last.push_back(std::move(row));
        if (last.size() > 20) {
            last.erase(last.begin());
        }
    }
    const bool bytesLimited = agentMaxBytes > 0 && renderedBytes > (uint64_t)agentMaxBytes;
    const bool capped = rowsLimited || bytesLimited;
    const bool sampled = capped && totalRows > 40;
    if (!sampled) {
        // Small enough to print in full: replay the first rows then the tail.
        if (totalRows <= 20) {
            for (auto& row : first) {
                printf("%s\n", row.text.c_str());
            }
        } else {
            queryResult.resetIterator();
            while (queryResult.hasNext()) {
                if (printInterrupted) {
                    return;
                }
                auto tuple = queryResult.getNext();
                std::string rowText = "|";
                for (auto i = 0u; i < numCols; i++) {
                    std::string cell;
                    if (tuple->getValue(i)->isNull()) {
                        cell = "NULL";
                    } else {
                        std::string raw = tuple->getValue(i)->toString();
                        if (agentMaxCellWidth > 0 &&
                            agent::utf8Length(raw) > (size_t)agentMaxCellWidth) {
                            size_t omitted = 0;
                            // NB: sequenced on purpose (see the first truncation loop above).
                            std::string kept =
                                agent::utf8Truncate(raw, (size_t)agentMaxCellWidth, omitted);
                            raw = kept + "…(+" + std::to_string(omitted) + " chars)";
                        }
                        cell = agent::markdownEscapeCell(raw);
                    }
                    rowText += " " + cell + " |";
                }
                printf("%s\n", rowText.c_str());
            }
        }
    } else {
        for (auto& row : first) {
            printf("%s\n", row.text.c_str());
        }
        std::string marker = "| … " + std::to_string(totalRows - 40) + " rows omitted … |";
        for (auto i = 1u; i < numCols; i++) {
            marker += " |";
        }
        printf("%s\n", marker.c_str());
        for (auto& row : last) {
            printf("%s\n", row.text.c_str());
        }
    }
    std::string hash = agent::hashHex(totalHash == 0 ? 1469598103934665603ULL : totalHash);
    if (sampled) {
        std::string limitHint = rowsLimited ? ".maxrows 0" : ".maxbytes 0";
        printf("first 20 and last 20 of %" PRIu64 " rows (%s for all), hash %s\n", totalRows,
            limitHint.c_str(), hash.c_str());
    } else if (capped) {
        std::string limitHint = rowsLimited ? ".maxrows 0" : ".maxbytes 0";
        printf("%" PRIu64 " rows (capped; %s for all), hash %s\n", totalRows, limitHint.c_str(),
            hash.c_str());
    } else if (totalRows == 0) {
        printf("empty result, hash %s\n", hash.c_str());
    } else if (totalRows >= 10) {
        printf("%" PRIu64 " rows, hash %s\n", totalRows, hash.c_str());
    }
}

void EmbeddedShell::printCompactExplain(QueryResult& queryResult) const {
    queryResult.resetIterator();
    if (!queryResult.hasNext()) {
        return;
    }
    std::string plan = queryResult.getNext()->toString();
    std::string compact = agent::compactPlan(plan);
    if (compact.empty()) {
        printf("%s", plan.c_str());
        return;
    }
    printf("%s", compact.c_str());
}

void EmbeddedShell::printAgentStatus() const {
    if (agentMode) {
        std::string id = agentEnv.empty() ? "flag" : agent::agentIdForEnv(agentEnv);
        if (agentAutoDetected && !agentEnv.empty()) {
            printf("agent: on (%s, auto-detected via %s)\n", id.c_str(), agentEnv.c_str());
        } else {
            printf("agent: on (%s)\n", id.c_str());
        }
    } else {
        printf("agent: off\n");
    }
    if (agentMode && !modeExplicitlySet) {
        printf("mode: markdown (agent compact)\n");
    }
    printf("stats: %s\n", stats ? "on" : "off");
    if (agentMode) {
        printf("maxrows: %lld, maxbytes: %lld, maxcellwidth: %lld (0/-1 = unlimited)\n",
            static_cast<long long>(agentMaxRows), static_cast<long long>(agentMaxBytes),
            static_cast<long long>(agentMaxCellWidth));
        printf("explain: %s\n", explainFormat.c_str());
    }
}

void EmbeddedShell::printAgentHelp() const {
    printf("Agent mode renders results and errors for AI coding agents.\n");
    printf("It turns on automatically when an agent env var (e.g. CLAUDECODE) is set,\n");
    printf("stdout is not a terminal, and no output-format flag was passed.\n");
    printf("--agent forces it on, --no_agent forces it off, LBUG_AGENT_MODE=1 forces it on.\n");
    printf("\n");
    printf("Output rules:\n");
    printf("  - results are Markdown tables, no padding; the header carries types\n");
    printf("    (| name:STRING | n:INT64 |, then |---|---|)\n");
    printf("  - at most 1000 rows / 10000 bytes print in full; beyond that the\n");
    printf("    first and last 20 rows are shown around an explicit omitted-rows\n");
    printf("    marker, plus a footer with a result hash\n");
    printf("  - long cell values are cut at 500 chars with a …(+N chars) marker\n");
    printf("  - errors go to stderr as JSON "
           "{\"exception_type\": ..., \"exception_message\": ...}\n");
    printf("  - EXPLAIN uses a compact one-operator-per-line plan\n");
    printf("\n");
    printf("Useful settings (explicit settings win over agent defaults):\n");
    printf("  .maxrows N / .maxbytes N (0/-1 = unlimited), .maxcellwidth N\n");
    printf("  .explain [compact|box|auto], .tables, .show (agent status)\n");
    printf("  CALL current_setting('timeout')/SET max_execution_time to bound runtime;\n");
    printf("  DESCRIBE <query> for result columns; SUMMARIZE for a quick profile.\n");
}

static std::string backtickQuote(const std::string& name) {
    std::string out = "`";
    for (char c : name) {
        if (c == '`') {
            out += "``";
        } else {
            out += c;
        }
    }
    out += "`";
    return out;
}

void EmbeddedShell::printAgentTables() {
    auto tablesResult = conn->query("CALL show_tables() RETURN *");
    if (!tablesResult->isSuccess()) {
        printErrorMessage("CALL show_tables() RETURN *", *tablesResult);
        return;
    }
    // Locate the name/type columns by name so internal renames stay working.
    auto tableColNames = tablesResult->getColumnNames();
    int nameIdx = -1, typeIdx = -1;
    for (auto i = 0u; i < tableColNames.size(); i++) {
        if (tableColNames[i] == "name") {
            nameIdx = (int)i;
        } else if (tableColNames[i] == "type") {
            typeIdx = (int)i;
        }
    }
    if (nameIdx < 0) {
        printErrorMessage("CALL show_tables() RETURN *", *tablesResult);
        return;
    }
    struct TableDesc {
        std::string name;
        std::string type;
    };
    std::vector<TableDesc> tables;
    while (tablesResult->hasNext()) {
        auto tuple = tablesResult->getNext();
        TableDesc desc;
        desc.name = tuple->getValue((uint32_t)nameIdx)->toString();
        desc.type = typeIdx >= 0 ? tuple->getValue((uint32_t)typeIdx)->toString() : "";
        tables.push_back(std::move(desc));
    }
    if (tables.empty()) {
        printf("(no tables)\n");
        return;
    }
    for (auto& table : tables) {
        std::string lowerType = table.type;
        std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(),
            [](unsigned char c) { return std::tolower(c); });
        bool isRel = lowerType.find("rel") != std::string::npos;
        // Approximate row count.
        std::string countQuery =
            isRel ? "MATCH ()-[r:" + backtickQuote(table.name) + "]->() RETURN COUNT(*) AS c" :
                    "MATCH (n:" + backtickQuote(table.name) + ") RETURN COUNT(*) AS c";
        std::string countStr = "~?";
        auto countResult = conn->query(countQuery);
        if (countResult->isSuccess() && countResult->hasNext()) {
            countStr = "~" + countResult->getNext()->getValue(0)->toString();
        }
        // Columns via table_info.
        std::string cols;
        std::string quotedName = table.name;
        {
            std::string escaped;
            for (char c : quotedName) {
                if (c == '\'') {
                    escaped += "''";
                } else {
                    escaped += c;
                }
            }
            quotedName = std::move(escaped);
        }
        auto infoResult = conn->query("CALL table_info('" + quotedName + "') RETURN *");
        if (infoResult->isSuccess()) {
            auto infoCols = infoResult->getColumnNames();
            int colNameIdx = -1, colTypeIdx = -1, pkIdx = -1;
            for (auto i = 0u; i < infoCols.size(); i++) {
                if (infoCols[i] == "name") {
                    colNameIdx = (int)i;
                } else if (infoCols[i] == "type") {
                    colTypeIdx = (int)i;
                } else if (infoCols[i] == "primary key") {
                    pkIdx = (int)i;
                }
            }
            bool firstCol = true;
            while (infoResult->hasNext()) {
                auto tuple = infoResult->getNext();
                if (!firstCol) {
                    cols += ", ";
                }
                firstCol = false;
                cols += tuple->getValue((uint32_t)colNameIdx)->toString() + " " +
                        tuple->getValue((uint32_t)colTypeIdx)->toString();
                if (pkIdx >= 0 && tuple->getValue((uint32_t)pkIdx)->toString() == "True") {
                    cols += " PK";
                }
            }
        }
        std::string typeLabel = lowerType.empty() ? "table" : lowerType;
        if (cols.empty()) {
            printf("%s (%s, %s rows)\n", table.name.c_str(), typeLabel.c_str(), countStr.c_str());
        } else {
            printf("%s (%s, %s rows): %s\n", table.name.c_str(), typeLabel.c_str(),
                countStr.c_str(), cols.c_str());
        }
    }
}

void EmbeddedShell::printSchema() {
    std::stringstream ss;
    auto clientContext = conn->getClientContext();
    bool transactionStarted = false;
    if (transaction::Transaction::Get(*clientContext) == NULL) {
        transaction::TransactionContext::Get(*clientContext)
            ->beginReadTransaction(); // start transaction to get schema
        transactionStarted = true;
    }
    auto extensionCypher = extension::ExtensionManager::Get(*clientContext)->toCypher();
    if (!extensionCypher.empty()) {
        ss << extensionCypher << std::endl;
    }
    const auto catalog = catalog::Catalog::Get(*clientContext);
    auto transaction = transaction::Transaction::Get(*clientContext);
    catalog::ToCypherInfo toCypherInfo;
    for (const auto& nodeTableEntry :
        catalog->getNodeTableEntries(transaction, false /* useInternal */)) {
        if (catalog::isSchemaGraphTableName(nodeTableEntry->getName())) {
            continue;
        }
        ss << nodeTableEntry->toCypher(toCypherInfo) << std::endl;
    }
    catalog::RelGroupToCypherInfo relTableToCypherInfo(clientContext);
    for (const auto& entry : catalog->getRelGroupEntries(transaction, false /* useInternal */)) {
        if (catalog::isSchemaGraphTableName(entry->getName())) {
            continue;
        }
        ss << entry->toCypher(relTableToCypherInfo) << std::endl;
    }
    catalog::RelGroupToCypherInfo relGroupToCypherInfo(clientContext);
    for (const auto sequenceEntry : catalog->getSequenceEntries(transaction)) {
        ss << sequenceEntry->toCypher(relGroupToCypherInfo) << std::endl;
    }
    for (auto macroName : catalog->getMacroNames(transaction)) {
        ss << catalog->getScalarMacroFunction(transaction, macroName)->toCypher(macroName)
           << std::endl;
    }
    if (transactionStarted) {
        transaction::TransactionContext::Get(*clientContext)->commit();
    }
    printf("%s", ss.str().c_str());
}

} // namespace main
} // namespace lbug
