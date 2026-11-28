; Keywords
[
  "fun"
  "ext"
  "struct"
  "enum"
  "const"
  "import"
  "var"
  "as"
  "return"
  "if"
  "else"
  "match"
  "for"
  "break"
  "continue"
  "defer"
  "or"
] @keyword

(visibility) @keyword.modifier

; Types
(primitive_type) @type.builtin
(type_identifier) @type
(struct_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(struct_literal type: (identifier) @type)
(struct_literal type: (member_expression member: (identifier) @type))

; Functions
(function_declaration name: (identifier) @function)
(extern_function_declaration name: (identifier) @function)
(call_expression function: (identifier) @function)
(call_expression function: (member_expression member: (identifier) @function.method))
(call_expression function: (generic_expression function: (identifier) @function))
(call_expression function: (generic_expression function: (member_expression member: (identifier) @function.method)))

; Variables, parameters, fields, constants
(parameter name: (identifier) @variable.parameter)
(field_declaration name: (identifier) @property)
(struct_field_init name: (identifier) @property)
(member_expression member: (identifier) @property)
(enum_member name: (identifier) @constant)
(const_declaration name: (identifier) @constant)

; Builtin-looking calls
((call_expression
  function: (identifier) @function.builtin)
  (#any-of? @function.builtin "len" "data" "size_of"))

; Labels
(for_statement label: (identifier) @label)
(break_statement label: (identifier) @label)
(continue_statement label: (identifier) @label)

; Literals
(boolean_literal) @boolean
(null_literal) @constant.builtin
(integer_literal) @number
(float_literal) @number.float
(string_literal) @string
(char_literal) @character
(escape_sequence) @string.escape

; Comments
(line_comment) @comment
(block_comment) @comment

; Operators & punctuation
[
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  "+" "-" "*" "/" "%"
  "==" "!=" "<" "<=" ">" ">="
  "&&" "||" "!"
  "&" "|" "^" "~" "<<" ">>"
  "=>" ".."
] @operator

[
  "(" ")" "{" "}" "[" "]"
] @punctuation.bracket

[
  "," ":" ";" "."
] @punctuation.delimiter
