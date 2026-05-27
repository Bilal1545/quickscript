#include "ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AstNode *ast_new(Arena *a, AstKind kind) {
    AstNode *n = (AstNode *)arena_alloc(a, sizeof(AstNode));
    n->kind = kind;
    return n;
}

void nl_push(Arena *a, NodeList *list, AstNode *child) {
    if (list->len == list->cap) {
        size_t nc = list->cap ? list->cap * 2 : 4;
        AstNode **arr = (AstNode **)arena_alloc(a, nc * sizeof(AstNode *));
        if (list->len) memcpy(arr, list->items, list->len * sizeof(AstNode *));
        list->items = arr;
        list->cap = nc;
    }
    list->items[list->len++] = child;
}

static void walk_list(NodeList *list, const AstVisitor *v);

void ast_walk(AstNode *n, const AstVisitor *v) {
    if (!n) return;
    if (v->enter && !v->enter(n, v->user)) {
        if (v->leave) v->leave(n, v->user);
        return;
    }
    switch (n->kind) {
        case AST_PROGRAM:
        case AST_BLOCK_STMT:
            walk_list(&n->block.body, v);
            break;
        case AST_VAR_DECL:
            walk_list(&n->var_decl.declarations, v);
            break;
        case AST_VAR_DECLARATOR:
            ast_walk(n->var_declarator.id, v);
            ast_walk(n->var_declarator.init, v);
            break;
        case AST_EXPR_STMT:
            ast_walk(n->expr_stmt.expr, v);
            break;
        case AST_IF_STMT:
            ast_walk(n->if_.test, v);
            ast_walk(n->if_.consequent, v);
            ast_walk(n->if_.alternate, v);
            break;
        case AST_WHILE_STMT:
        case AST_DO_WHILE_STMT:
            ast_walk(n->while_.test, v);
            ast_walk(n->while_.body, v);
            break;
        case AST_FOR_STMT:
            ast_walk(n->for_.init, v);
            ast_walk(n->for_.test, v);
            ast_walk(n->for_.update, v);
            ast_walk(n->for_.body, v);
            break;
        case AST_FOR_IN_STMT:
        case AST_FOR_OF_STMT:
            ast_walk(n->for_each.left, v);
            ast_walk(n->for_each.right, v);
            ast_walk(n->for_each.body, v);
            break;
        case AST_SWITCH_STMT:
            ast_walk(n->switch_.discriminant, v);
            walk_list(&n->switch_.cases, v);
            break;
        case AST_SWITCH_CASE:
            ast_walk(n->switch_case.test, v);
            walk_list(&n->switch_case.consequent, v);
            break;
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
            ast_walk(n->break_continue.label, v);
            break;
        case AST_RETURN_STMT:
        case AST_THROW_STMT:
            ast_walk(n->unary_stmt.arg, v);
            break;
        case AST_TRY_STMT:
            ast_walk(n->try_.block, v);
            ast_walk(n->try_.handler, v);
            ast_walk(n->try_.finalizer, v);
            break;
        case AST_CATCH_CLAUSE:
            ast_walk(n->catch_.param, v);
            ast_walk(n->catch_.body, v);
            break;
        case AST_FN_DECL:
        case AST_FN_EXPR:
        case AST_ARROW_FN_EXPR:
            ast_walk(n->func.id, v);
            walk_list(&n->func.params, v);
            ast_walk(n->func.body, v);
            break;
        case AST_CLASS_DECL:
            ast_walk(n->class_.id, v);
            ast_walk(n->class_.super_class, v);
            ast_walk(n->class_.body, v);
            break;
        case AST_CLASS_BODY:
            walk_list(&n->class_body.body, v);
            break;
        case AST_METHOD_DEF:
            ast_walk(n->method_def.key, v);
            ast_walk(n->method_def.value, v);
            break;
        case AST_LABELED_STMT:
            ast_walk(n->labeled.label, v);
            ast_walk(n->labeled.body, v);
            break;
        case AST_BINARY_EXPR:
        case AST_LOGICAL_EXPR:
            ast_walk(n->binary.left, v);
            ast_walk(n->binary.right, v);
            break;
        case AST_UNARY_EXPR:
            ast_walk(n->unary.arg, v);
            break;
        case AST_UPDATE_EXPR:
            ast_walk(n->update.arg, v);
            break;
        case AST_ASSIGN_EXPR:
            ast_walk(n->assign.left, v);
            ast_walk(n->assign.right, v);
            break;
        case AST_MEMBER_EXPR:
            ast_walk(n->member.object, v);
            ast_walk(n->member.property, v);
            break;
        case AST_CALL_EXPR:
        case AST_NEW_EXPR:
            ast_walk(n->call.callee, v);
            walk_list(&n->call.args, v);
            break;
        case AST_CONDITIONAL_EXPR:
            ast_walk(n->conditional.test, v);
            ast_walk(n->conditional.consequent, v);
            ast_walk(n->conditional.alternate, v);
            break;
        case AST_ARRAY_EXPR:
            walk_list(&n->array.elements, v);
            break;
        case AST_OBJECT_EXPR:
            walk_list(&n->object.properties, v);
            break;
        case AST_PROPERTY:
            ast_walk(n->property.key, v);
            ast_walk(n->property.value, v);
            break;
        case AST_TEMPLATE_LITERAL:
            walk_list(&n->template_.quasis, v);
            walk_list(&n->template_.expressions, v);
            break;
        case AST_TAGGED_TEMPLATE_EXPR:
            ast_walk(n->tagged_template.tag, v);
            ast_walk(n->tagged_template.quasi, v);
            break;
        case AST_SEQUENCE_EXPR:
            walk_list(&n->sequence.expressions, v);
            break;
        case AST_CHAIN_EXPR:
            ast_walk(n->chain.expr, v);
            break;
        case AST_SPREAD_ELEMENT:
        case AST_REST_ELEMENT:
            ast_walk(n->spread_or_rest.arg, v);
            break;
        case AST_YIELD_EXPR:
        case AST_AWAIT_EXPR:
            ast_walk(n->yield_await.arg, v);
            break;
        case AST_PAREN_EXPR:
            ast_walk(n->paren.expr, v);
            break;
        case AST_META_PROPERTY:
            ast_walk(n->meta_property.meta, v);
            ast_walk(n->meta_property.property, v);
            break;
        case AST_ARRAY_PATTERN:
            walk_list(&n->array_pattern.elements, v);
            break;
        case AST_OBJECT_PATTERN:
            walk_list(&n->object_pattern.properties, v);
            break;
        case AST_ASSIGN_PATTERN:
            ast_walk(n->assign_pattern.left, v);
            ast_walk(n->assign_pattern.right, v);
            break;
        case AST_IMPORT_DECL:
            walk_list(&n->import_decl.specifiers, v);
            break;
        case AST_IMPORT_SPECIFIER:
        case AST_IMPORT_DEFAULT_SPECIFIER:
        case AST_IMPORT_NAMESPACE_SPECIFIER:
            ast_walk(n->import_specifier.imported, v);
            ast_walk(n->import_specifier.local, v);
            break;
        case AST_EXPORT_NAMED_DECL:
            ast_walk(n->export_named.declaration, v);
            walk_list(&n->export_named.specifiers, v);
            break;
        case AST_EXPORT_DEFAULT_DECL:
            ast_walk(n->export_default.declaration, v);
            break;
        case AST_EXPORT_ALL_DECL:
            ast_walk(n->export_all.exported_name, v);
            break;
        case AST_EXPORT_SPECIFIER:
            ast_walk(n->export_specifier.local, v);
            ast_walk(n->export_specifier.exported, v);
            break;
        default:
            /* leaf or no children: LITERAL, IDENTIFIER, THIS_EXPR, EMPTY_STMT,
             * DEBUGGER_STMT, TEMPLATE_ELEMENT — nothing to recurse into. */
            break;
    }
    if (v->leave) v->leave(n, v->user);
}

static void walk_list(NodeList *list, const AstVisitor *v) {
    for (size_t i = 0; i < list->len; ++i) ast_walk(list->items[i], v);
}

const char *ast_kind_name(AstKind k) {
    static const char *NAMES[AST_COUNT] = {0};
    if (!NAMES[AST_PROGRAM]) {
        NAMES[AST_PROGRAM] = "Program";
        NAMES[AST_IMPORT_DECL] = "ImportDeclaration";
        NAMES[AST_IMPORT_SPECIFIER] = "ImportSpecifier";
        NAMES[AST_IMPORT_DEFAULT_SPECIFIER] = "ImportDefaultSpecifier";
        NAMES[AST_IMPORT_NAMESPACE_SPECIFIER] = "ImportNamespaceSpecifier";
        NAMES[AST_EXPORT_NAMED_DECL] = "ExportNamedDeclaration";
        NAMES[AST_EXPORT_DEFAULT_DECL] = "ExportDefaultDeclaration";
        NAMES[AST_EXPORT_ALL_DECL] = "ExportAllDeclaration";
        NAMES[AST_EXPORT_SPECIFIER] = "ExportSpecifier";
        NAMES[AST_BLOCK_STMT] = "BlockStatement";
        NAMES[AST_VAR_DECL] = "VariableDeclaration";
        NAMES[AST_VAR_DECLARATOR] = "VariableDeclarator";
        NAMES[AST_EXPR_STMT] = "ExpressionStatement";
        NAMES[AST_IF_STMT] = "IfStatement";
        NAMES[AST_WHILE_STMT] = "WhileStatement";
        NAMES[AST_DO_WHILE_STMT] = "DoWhileStatement";
        NAMES[AST_FOR_STMT] = "ForStatement";
        NAMES[AST_FOR_IN_STMT] = "ForInStatement";
        NAMES[AST_FOR_OF_STMT] = "ForOfStatement";
        NAMES[AST_SWITCH_STMT] = "SwitchStatement";
        NAMES[AST_SWITCH_CASE] = "SwitchCase";
        NAMES[AST_BREAK_STMT] = "BreakStatement";
        NAMES[AST_CONTINUE_STMT] = "ContinueStatement";
        NAMES[AST_RETURN_STMT] = "ReturnStatement";
        NAMES[AST_THROW_STMT] = "ThrowStatement";
        NAMES[AST_TRY_STMT] = "TryStatement";
        NAMES[AST_CATCH_CLAUSE] = "CatchClause";
        NAMES[AST_FN_DECL] = "FunctionDeclaration";
        NAMES[AST_CLASS_DECL] = "ClassDeclaration";
        NAMES[AST_CLASS_BODY] = "ClassBody";
        NAMES[AST_METHOD_DEF] = "MethodDefinition";
        NAMES[AST_EMPTY_STMT] = "EmptyStatement";
        NAMES[AST_LABELED_STMT] = "LabeledStatement";
        NAMES[AST_DEBUGGER_STMT] = "DebuggerStatement";
        NAMES[AST_LITERAL] = "Literal";
        NAMES[AST_IDENTIFIER] = "Identifier";
        NAMES[AST_THIS_EXPR] = "ThisExpression";
        NAMES[AST_BINARY_EXPR] = "BinaryExpression";
        NAMES[AST_LOGICAL_EXPR] = "LogicalExpression";
        NAMES[AST_UNARY_EXPR] = "UnaryExpression";
        NAMES[AST_UPDATE_EXPR] = "UpdateExpression";
        NAMES[AST_ASSIGN_EXPR] = "AssignmentExpression";
        NAMES[AST_MEMBER_EXPR] = "MemberExpression";
        NAMES[AST_CALL_EXPR] = "CallExpression";
        NAMES[AST_NEW_EXPR] = "NewExpression";
        NAMES[AST_CONDITIONAL_EXPR] = "ConditionalExpression";
        NAMES[AST_ARRAY_EXPR] = "ArrayExpression";
        NAMES[AST_OBJECT_EXPR] = "ObjectExpression";
        NAMES[AST_PROPERTY] = "Property";
        NAMES[AST_ARROW_FN_EXPR] = "ArrowFunctionExpression";
        NAMES[AST_FN_EXPR] = "FunctionExpression";
        NAMES[AST_TEMPLATE_LITERAL] = "TemplateLiteral";
        NAMES[AST_TEMPLATE_ELEMENT] = "TemplateElement";
        NAMES[AST_TAGGED_TEMPLATE_EXPR] = "TaggedTemplateExpression";
        NAMES[AST_SEQUENCE_EXPR] = "SequenceExpression";
        NAMES[AST_CHAIN_EXPR] = "ChainExpression";
        NAMES[AST_SPREAD_ELEMENT] = "SpreadElement";
        NAMES[AST_YIELD_EXPR] = "YieldExpression";
        NAMES[AST_AWAIT_EXPR] = "AwaitExpression";
        NAMES[AST_META_PROPERTY] = "MetaProperty";
        NAMES[AST_PAREN_EXPR] = "ParenthesizedExpression";
        NAMES[AST_ARRAY_PATTERN] = "ArrayPattern";
        NAMES[AST_OBJECT_PATTERN] = "ObjectPattern";
        NAMES[AST_REST_ELEMENT] = "RestElement";
        NAMES[AST_ASSIGN_PATTERN] = "AssignmentPattern";
    }
    if (k < 0 || k >= AST_COUNT) return "?";
    return NAMES[k] ? NAMES[k] : "?";
}
