#include "parser/transformer.h"

#include <cstdlib>

#include "common/assert.h"
#include "common/exception/parser.h"
#include "extension/transformer_extension.h"
#include "parser/explain_statement.h"
#include "parser/graph_statement.h"
#include "parser/query/regular_query.h" // IWYU pragma: keep (fixes a forward declaration error)

using namespace lbug::common;

namespace lbug {
namespace parser {

std::vector<std::shared_ptr<Statement>> Transformer::transform() {
    std::vector<std::shared_ptr<Statement>> statements;
    for (auto& oc_Statement : root.oC_Cypher()) {
        auto statement = transformStatement(*oc_Statement->oC_Statement());
        if (oc_Statement->oC_AnyCypherOption()) {
            auto cypherOption = oc_Statement->oC_AnyCypherOption();
            auto explainType = ExplainType::PROFILE;
            if (cypherOption->oC_Explain()) {
                explainType = cypherOption->oC_Explain()->LOGICAL() ? ExplainType::LOGICAL_PLAN :
                                                                      ExplainType::PHYSICAL_PLAN;
            }
            statements.push_back(
                std::make_unique<ExplainStatement>(std::move(statement), explainType));
            continue;
        }
        statements.push_back(std::move(statement));
    }
    return statements;
}

std::unique_ptr<Statement> Transformer::transformStatement(CypherParser::OC_StatementContext& ctx) {
    if (ctx.oC_Query()) {
        return transformQuery(*ctx.oC_Query());
    } else if (ctx.iC_CreateNodeTable()) {
        return transformCreateNodeTable(*ctx.iC_CreateNodeTable());
    } else if (ctx.iC_CreateRelTable()) {
        return transformCreateRelGroup(*ctx.iC_CreateRelTable());
    } else if (ctx.iC_CreateSequence()) {
        return transformCreateSequence(*ctx.iC_CreateSequence());
    } else if (ctx.iC_CreateType()) {
        return transformCreateType(*ctx.iC_CreateType());
    } else if (ctx.iC_CreateUser()) {
        return transformExtensionStatement(ctx.iC_CreateUser());
    } else if (ctx.iC_CreateRole()) {
        return transformExtensionStatement(ctx.iC_CreateRole());
    } else if (ctx.iC_Drop()) {
        return transformDrop(*ctx.iC_Drop());
    } else if (ctx.iC_AlterTable()) {
        return transformAlterTable(*ctx.iC_AlterTable());
    } else if (ctx.iC_CopyFromByColumn()) {
        return transformCopyFromByColumn(*ctx.iC_CopyFromByColumn());
    } else if (ctx.iC_CopyFrom()) {
        return transformCopyFrom(*ctx.iC_CopyFrom());
    } else if (ctx.iC_CopyTO()) {
        return transformCopyTo(*ctx.iC_CopyTO());
    } else if (ctx.iC_StandaloneCall()) {
        return transformStandaloneCall(*ctx.iC_StandaloneCall());
    } else if (ctx.iC_CreateMacro()) {
        return transformCreateMacro(*ctx.iC_CreateMacro());
    } else if (ctx.iC_CommentOn()) {
        return transformCommentOn(*ctx.iC_CommentOn());
    } else if (ctx.iC_Transaction()) {
        return transformTransaction(*ctx.iC_Transaction());
    } else if (ctx.iC_Extension()) {
        return transformExtension(*ctx.iC_Extension());
    } else if (ctx.iC_ExportDatabase()) {
        return transformExportDatabase(*ctx.iC_ExportDatabase());
    } else if (ctx.iC_ImportDatabase()) {
        return transformImportDatabase(*ctx.iC_ImportDatabase());
    } else if (ctx.iC_AttachDatabase()) {
        return transformAttachDatabase(*ctx.iC_AttachDatabase());
    } else if (ctx.iC_DetachDatabase()) {
        return transformDetachDatabase(*ctx.iC_DetachDatabase());
    } else if (ctx.iC_UseDatabase()) {
        return transformUseDatabase(*ctx.iC_UseDatabase());
    } else if (ctx.iC_CreateGraph()) {
        return transformCreateGraph(*ctx.iC_CreateGraph());
    } else if (ctx.iC_UseGraph()) {
        return transformUseGraph(*ctx.iC_UseGraph());
    } else {
        UNREACHABLE_CODE;
    }
}

std::unique_ptr<ParsedExpression> Transformer::transformWhere(CypherParser::OC_WhereContext& ctx) {
    return transformExpression(*ctx.oC_Expression());
}

std::string Transformer::transformSchemaName(CypherParser::OC_SchemaNameContext& ctx) {
    auto symbolicNames = ctx.oC_SymbolicName();
    if (symbolicNames.size() == 1) {
        return transformSymbolicName(*symbolicNames[0]);
    }
    // Qualified name: db.table
    return transformSymbolicName(*symbolicNames[0]) + "." +
           transformSymbolicName(*symbolicNames[1]);
}

std::string Transformer::transformStringLiteral(antlr4::tree::TerminalNode& stringLiteral) {
    auto str = stringLiteral.getText();
    std::string content = str.substr(1, str.length() - 2);
    std::string result;
    result.reserve(content.length());
    for (auto i = 0u; i < content.length(); i++) {
        if (content[i] == '\\' && i + 1 < content.length()) {
            char next = content[i + 1];
            switch (next) {
            case '\\':
            case '\'':
            case '"': {
                result += next;
                i++;
            } break;
            case 'b':
            case 'B': {
                result += '\b';
                i++;
            } break;
            case 'f':
            case 'F': {
                result += '\f';
                i++;
            } break;
            case 'n':
            case 'N': {
                result += '\n';
                i++;
            } break;
            case 'r':
            case 'R': {
                result += '\r';
                i++;
            } break;
            case 't':
            case 'T': {
                result += '\t';
                i++;
            } break;
            case 'x':
            case 'X': {
                result += content.substr(i, 4);
                i += 3;
            } break;
            case 'u':
            case 'U': {
                // Handle \uHHHH and \UHHHHHHHH unicode escape sequences
                if (next == 'u' || next == 'U') {
                    int hexDigits = (next == 'u') ? 4 : 8;
                    if (i + 1 + hexDigits > content.length()) {
                        UNREACHABLE_CODE;
                    }
                    std::string hexStr = content.substr(i + 2, hexDigits);
                    char* endPtr = nullptr;
                    long hexValue = std::strtol(hexStr.c_str(), &endPtr, 16);
                    if (endPtr != hexStr.c_str() + hexDigits) {
                        UNREACHABLE_CODE;
                    }
                    // Convert Unicode code point to UTF-8
                    if (hexValue <= 0x7F) {
                        result += static_cast<char>(hexValue);
                    } else if (hexValue <= 0x7FF) {
                        result += static_cast<char>(0xC0 | (hexValue >> 6));
                        result += static_cast<char>(0x80 | (hexValue & 0x3F));
                    } else if (hexValue <= 0xFFFF) {
                        result += static_cast<char>(0xE0 | (hexValue >> 12));
                        result += static_cast<char>(0x80 | ((hexValue >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (hexValue & 0x3F));
                    } else if (hexValue <= 0x10FFFF) {
                        result += static_cast<char>(0xF0 | (hexValue >> 18));
                        result += static_cast<char>(0x80 | ((hexValue >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((hexValue >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (hexValue & 0x3F));
                    } else {
                        UNREACHABLE_CODE;
                    }
                    i += 1 + hexDigits;
                }
            } break;
            default:
                UNREACHABLE_CODE;
            }
        } else {
            result += content[i];
        }
    }

    return result;
}

std::string Transformer::transformVariable(CypherParser::OC_VariableContext& ctx) {
    return transformSymbolicName(*ctx.oC_SymbolicName());
}
std::string Transformer::transformSymbolicName(CypherParser::OC_SymbolicNameContext& ctx) {
    if (ctx.EscapedSymbolicName()) {
        std::string escapedSymbolName = ctx.EscapedSymbolicName()->getText();
        // escapedSymbolName symbol will be of form "`Some.Value`". Therefore, we need to sanitize
        // it such that we don't store the symbol with escape character.
        return escapedSymbolName.substr(1, escapedSymbolName.size() - 2);
    } else {
        DASSERT(ctx.HexLetter() || ctx.UnescapedSymbolicName() || ctx.iC_NonReservedKeywords());
        return ctx.getText();
    }
}

std::unique_ptr<Statement> Transformer::transformExtensionStatement(
    antlr4::ParserRuleContext* ctx) {
    for (auto& transformerExtension : transformerExtensions) {
        auto statement = transformerExtension->transform(ctx);
        if (statement) {
            return statement;
        }
    }
    throw common::ParserException{
        "Failed parse the statement. Do you forget to load the extension?"};
}

} // namespace parser
} // namespace lbug
