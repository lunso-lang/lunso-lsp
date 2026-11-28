(function_declaration
  "fun" @context
  name: (identifier) @name) @item

(extern_function_declaration
  "ext" @context
  "fun" @context
  name: (identifier) @name) @item

(struct_declaration
  "struct" @context
  name: (identifier) @name) @item

(enum_declaration
  "enum" @context
  name: (identifier) @name) @item

(trait_declaration
  "trait" @context
  name: (identifier) @name) @item

(const_declaration
  "const" @context
  name: (identifier) @name) @item
