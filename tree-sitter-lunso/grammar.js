/**
 * @file Lunso grammar for tree-sitter
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const PREC = {
  ASSIGN: 1,
  OR: 2,
  AND: 3,
  BIT_OR: 4,
  BIT_XOR: 5,
  BIT_AND: 6,
  EQ: 7,
  CMP: 8,
  SHIFT: 9,
  ADD: 10,
  MUL: 11,
  CAST: 12,
  UNARY: 13,
  POSTFIX: 14,
};

module.exports = grammar({
  name: 'lunso',

  extras: $ => [
    /[ \t\r\n]+/,
    $.line_comment,
    $.block_comment,
  ],

  word: $ => $.identifier,

  conflicts: $ => [
    [$._expression, $.struct_literal],
  ],

  rules: {
    source_file: $ => repeat(seq($._top_level, optional(';'))),

    _top_level: $ => choice(
      $.import_declaration,
      $.function_declaration,
      $.extern_function_declaration,
      $.struct_declaration,
      $.enum_declaration,
      $.const_declaration,
    ),

    visibility: _ => choice('pub', 'priv'),

    import_declaration: $ => seq(
      'import',
      field('path', $.module_path),
      optional(choice(
        seq('as', field('alias', $.identifier)),
        $.import_list,
      )),
    ),

    module_path: $ => seq(
      $.identifier,
      repeat(seq('.', $.identifier)),
      optional(seq('.', '*')),
    ),

    import_list: $ => seq(
      '{',
      optional(seq(
        $.import_item,
        repeat(seq(',', $.import_item)),
        optional(','),
      )),
      '}',
    ),

    import_item: $ => seq(
      field('name', $.identifier),
      optional(seq('as', field('alias', $.identifier))),
    ),

    type_parameter_list: $ => seq(
      '[',
      alias($.identifier, $.type_identifier),
      repeat(seq(',', alias($.identifier, $.type_identifier))),
      optional(','),
      ']',
    ),

    type_argument_list: $ => seq(
      '[',
      $._type,
      repeat(seq(',', $._type)),
      optional(','),
      ']',
    ),

    function_declaration: $ => seq(
      optional($.visibility),
      'fun',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameter_list)),
      field('parameters', $.parameter_list),
      optional(seq(':', field('return_type', $._type))),
      field('body', $.block),
    ),

    extern_function_declaration: $ => seq(
      optional($.visibility),
      'ext',
      'fun',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameter_list)),
      field('parameters', $.parameter_list),
      optional(seq(':', field('return_type', $._type))),
    ),

    parameter_list: $ => seq(
      '(',
      optional(seq(
        $.parameter,
        repeat(seq(',', $.parameter)),
        optional(','),
      )),
      ')',
    ),

    parameter: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    struct_declaration: $ => seq(
      optional($.visibility),
      'struct',
      field('name', $.identifier),
      optional(field('type_parameters', $.type_parameter_list)),
      '{',
      repeat(seq($.field_declaration, optional(';'))),
      '}',
    ),

    field_declaration: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
    ),

    enum_declaration: $ => seq(
      optional($.visibility),
      'enum',
      field('name', $.identifier),
      ':',
      field('backing_type', $._type),
      '{',
      repeat(seq($.enum_member, optional(choice(',', ';')))),
      '}',
    ),

    enum_member: $ => seq(
      field('name', $.identifier),
      optional(seq('=', optional('-'), choice($.integer_literal, $.char_literal))),
    ),

    const_declaration: $ => seq(
      optional($.visibility),
      'const',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=',
      field('value', $._expression),
    ),

    _type: $ => choice(
      $.primitive_type,
      $.pointer_type,
      $.array_type,
      $.slice_type,
      $.generic_type,
      $.qualified_type,
      alias($.identifier, $.type_identifier),
    ),

    primitive_type: _ => choice(
      'int',
      'i8', 'i16', 'i32', 'i64',
      'u8', 'u16', 'u32', 'u64',
      'f32', 'f64', 'bool',
    ),

    pointer_type: $ => seq('*', optional('const'), $._type),
    array_type: $ => seq('[', choice($.integer_literal, $.identifier), ']', $._type),
    slice_type: $ => seq('[', ']', $._type),
    qualified_type: $ => prec(1, seq($.identifier, repeat1(seq('.', $.identifier)))),
    generic_type: $ => prec(2, seq(
      choice($.qualified_type, alias($.identifier, $.type_identifier)),
      $.type_argument_list,
    )),

    block: $ => seq(
      '{',
      repeat(seq($._statement, optional(';'))),
      '}',
    ),

    _statement: $ => choice(
      $.var_statement,
      $.const_declaration,
      $.return_statement,
      $.if_statement,
      $.match_statement,
      $.for_statement,
      $.break_statement,
      $.continue_statement,
      $._expression,
    ),

    var_statement: $ => seq(
      'var',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('initializer', $._expression))),
    ),

    return_statement: $ => prec.right(seq('return', optional($._expression))),

    if_statement: $ => prec.right(seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.if_statement, $.block)))),
    )),

    match_statement: $ => seq(
      'match',
      field('value', $._expression),
      '{',
      repeat(seq($.match_arm, optional(';'))),
      '}',
    ),

    match_arm: $ => seq(
      choice(
        'else',
        seq($._expression, repeat(seq(',', $._expression))),
      ),
      '=>',
      choice($.block, $._statement),
    ),

    for_statement: $ => seq(
      optional(seq(field('label', $.identifier), ':')),
      'for',
      optional(choice(
        field('condition', $._expression),
        seq(
          optional(choice($.var_statement, $._expression)),
          ';',
          optional($._expression),
          ';',
          optional($._expression),
        ),
      )),
      field('body', $.block),
    ),

    break_statement: $ => prec.right(seq('break', optional(field('label', $.identifier)))),
    continue_statement: $ => prec.right(seq('continue', optional(field('label', $.identifier)))),

    _expression: $ => choice(
      $.assignment_expression,
      $.binary_expression,
      $.unary_expression,
      $.cast_expression,
      $.call_expression,
      $.generic_expression,
      $.index_expression,
      $.slice_expression,
      $.member_expression,
      $.struct_literal,
      $.array_literal,
      $.parenthesized_expression,
      $.boolean_literal,
      $.integer_literal,
      $.float_literal,
      $.string_literal,
      $.char_literal,
      $.identifier,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    assignment_expression: $ => prec.right(PREC.ASSIGN, seq(
      field('left', $._expression),
      field('operator', choice(
        '=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=',
      )),
      field('right', $._expression),
    )),

    binary_expression: $ => choice(
      ...[
        ['||', PREC.OR],
        ['&&', PREC.AND],
        ['|', PREC.BIT_OR],
        ['^', PREC.BIT_XOR],
        ['&', PREC.BIT_AND],
        ['==', PREC.EQ],
        ['!=', PREC.EQ],
        ['<', PREC.CMP],
        ['<=', PREC.CMP],
        ['>', PREC.CMP],
        ['>=', PREC.CMP],
        ['<<', PREC.SHIFT],
        ['>>', PREC.SHIFT],
        ['+', PREC.ADD],
        ['-', PREC.ADD],
        ['*', PREC.MUL],
        ['/', PREC.MUL],
        ['%', PREC.MUL],
      ].map(([op, p]) => prec.left(p, seq(
        field('left', $._expression),
        field('operator', op),
        field('right', $._expression),
      ))),
    ),

    unary_expression: $ => prec.right(PREC.UNARY, seq(
      field('operator', choice('+', '-', '!', '~', '&', '*')),
      field('operand', $._expression),
    )),

    cast_expression: $ => prec.left(PREC.CAST, seq(
      field('value', $._expression),
      'as',
      field('type', $._type),
    )),

    call_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('function', $._expression),
      '(',
      optional(seq($._expression, repeat(seq(',', $._expression)), optional(','))),
      ')',
    )),

    index_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    slice_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._expression),
      '[',
      field('start', $._expression),
      '..',
      field('end', $._expression),
      ']',
    )),

    member_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('object', $._expression),
      '.',
      field('member', $.identifier),
    )),

    generic_expression: $ => prec.left(PREC.POSTFIX, seq(
      field('function', $._expression),
      '.',
      field('type_arguments', $.type_argument_list),
    )),

    struct_literal: $ => prec.dynamic(-1, seq(
      field('type', choice($.identifier, $.member_expression)),
      optional(field('type_arguments', $.type_argument_list)),
      '{',
      optional(seq(
        $.struct_field_init,
        repeat(seq(',', $.struct_field_init)),
        optional(','),
      )),
      '}',
    )),

    struct_field_init: $ => seq(
      field('name', $.identifier),
      ':',
      field('value', $._expression),
    ),

    array_literal: $ => seq(
      '[',
      $._expression,
      repeat(seq(',', $._expression)),
      optional(','),
      ']',
    ),

    boolean_literal: _ => choice('true', 'false'),
    identifier: _ => /[a-zA-Z_][a-zA-Z0-9_]*/,
    integer_literal: _ => choice(
      /0[xX][0-9a-fA-F]+/,
      /0[bB][01]+/,
      /[0-9]+/,
    ),
    float_literal: _ => /[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+/,
    string_literal: $ => seq(
      '"',
      repeat(choice(
        /[^"\\\r\n]+/,
        $.escape_sequence,
      )),
      '"',
    ),
    char_literal: $ => seq(
      '\'',
      choice(
        /[^'\\\r\n]/,
        $.escape_sequence,
      ),
      '\'',
    ),
    escape_sequence: _ => /\\(0|n|r|t|\\|"|'|x[0-9a-fA-F]{2})/,
    line_comment: _ => /\/\/[^\r\n]*/,
    block_comment: _ => /\/\*([^*]|\*+[^*/])*\*+\//,
  },
});
