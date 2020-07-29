#include "compiler/lexer.h"

#include <map>
#include <utility>

#include "auto/compiler/keywords_set.hpp"
#include "common/algorithms/find.h"
#include "common/smart_ptrs/singleton.h"

#include "compiler/stage.h"
#include "compiler/threading/thread-id.h"

/***
  LexerData
 ***/
void LexerData::new_line() {
  line_num++;
}

LexerData::LexerData(vk::string_view new_code) :
  code(new_code.data()),
  code_end(code + new_code.size()),
  code_len(new_code.size()) {
  new_line();
  tokens.reserve(static_cast<size_t >(code_len * 0.3));
}

const char *LexerData::get_code() {
  return code;
}

void LexerData::pass(int shift) {
  line_num += std::count_if(code, code + shift, [](char c) { return c == '\n'; });
  pass_raw(shift);
}

void LexerData::pass_raw(int shift) {
  code += shift;
}

template <typename ...Args>
void LexerData::add_token_(int shift, Args&& ...tok) {
  kphp_assert (code + shift <= code_end);
  tokens.emplace_back(std::forward<Args>(tok)...);
  tokens.back().line_num = line_num;
  tokens.back().debug_str = vk::string_view(code, code + shift);
  //fprintf (stderr, "[%d] %.*s : %d\n", tok->type(), tok->debug_str.length(), tok->debug_str.begin(), line_num);
  pass(shift);
  hack_last_tokens();
}

template <typename ...Args>
void LexerData::add_token(int shift, Args&& ...tok) {
  flush_str();
  add_token_(shift, std::forward<Args>(tok)...);
}

void LexerData::start_str() {
  in_gen_str = true;
  str_begin = get_code();
  str_cur = get_code();
}

/**
 * append_char and flush_str are used to modify entities in PHP source code like string literals
 * e.g. we have to tokenize this code: $x = "New\n";
 * but in class-field `std::string SrcFile::text` we have R"($x = \"New\\n\";)"
 * we will replace "\\n" with "\n" and get string_view on this representation from text field
 *
 * It's really strange behaviour to modify source text, it's better to have dedicated tokens for them
 * which contains `std::string` not string_view, we will do it later
 */
void LexerData::append_char(int c) {
  if (!in_gen_str) {
    start_str();
  }
  if (c == -1) {
    int c = *get_code();
    *const_cast<char *>(str_cur++) = (char)c;
    if (c == '\n') {
      new_line();
    }
    pass_raw(1);
  } else {
    *const_cast<char *>(str_cur++) = (char)c;
  }
}

void LexerData::flush_str() {
  if (in_gen_str) {
    add_token_(0, tok_str, str_begin, str_cur);
    while (str_cur != get_code()) {
      *const_cast<char *>(str_cur++) = ' ';
    }
    in_gen_str = false;
  }
}

bool LexerData::are_last_tokens() {
  return true;
}

template<typename ...Args>
bool LexerData::are_last_tokens(TokenType type1, Args ...args) {
  return tokens.size() >= (sizeof...(args) + 1) &&
         tokens[tokens.size() - sizeof...(args) - 1].type() == type1 &&
         are_last_tokens(args...);
}

template<typename ...Args>
bool LexerData::are_last_tokens(any_token_tag, Args ...args) {
  return tokens.size() >= (sizeof...(args) + 1) &&
         are_last_tokens(args...);
}

template<TokenType token, typename ...Args>
bool LexerData::are_last_tokens(except_token_tag<token>, Args ...args) {
  return tokens.size() >= (sizeof...(args) + 1) &&
         tokens[tokens.size() - sizeof...(args) - 1].type() != token &&
         are_last_tokens(args...);
}


void LexerData::hack_last_tokens() {
  if (dont_hack_last_tokens) {
    return;
  }

  TokenType casts[][2] = {
    {tok_int,    tok_conv_int},
    {tok_float,  tok_conv_float},
    {tok_string, tok_conv_string},
    {tok_array,  tok_conv_array},
    {tok_object, tok_conv_object},
    {tok_bool,   tok_conv_bool},
    {tok_var,    tok_conv_var},
  };

  auto remove_last_tokens = [this](size_t cnt) {
    tokens.erase(std::prev(tokens.end(), cnt), tokens.end());
  };

  for (auto &cast : casts) {
    if (are_last_tokens(tok_oppar, cast[0], tok_clpar)) {
      remove_last_tokens(3);
      tokens.emplace_back(cast[1]);
      return;
    }
  }

  if (are_last_tokens(tok_elseif)) {
    tokens.back() = Token{tok_else};
    tokens.emplace_back(tok_if);
    return;
  }

  if (are_last_tokens(tok_str_begin, tok_str_end)) {
    remove_last_tokens(2);
    tokens.emplace_back(tok_str);
    return;
  }

  if (are_last_tokens(tok_str_begin, tok_str, tok_str_end)) {
    tokens.pop_back();
    tokens.erase(std::prev(tokens.end(), 2));
    return;
  }

  if (are_last_tokens(tok_new, tok_func_name, tok_oppar, any_token_tag{})) {
    auto class_name = tokens[tokens.size() - 3].str_val;
    if (class_name == "Exception" || class_name == "\\Exception") {
      Token t = tokens.back();
      tokens.pop_back();
      tokens.emplace_back(tok_file_c);
      tokens.emplace_back(tok_comma);
      tokens.emplace_back(tok_line_c);
      if (t.type() != tok_clpar) {
        tokens.emplace_back(tok_comma);
      }
      tokens.push_back(t);
      return;
    }
  }

  if (are_last_tokens(tok_new, tok_func_name, except_token_tag<tok_oppar>{})) {
    Token t = tokens.back();
    tokens.pop_back();
    tokens.emplace_back(tok_oppar);
    tokens.emplace_back(tok_clpar);
    tokens.push_back(t);
    return;
  }

  if (are_last_tokens(tok_new, tok_static)) {
    tokens.back().type_ = tok_func_name;
  }

  if (are_last_tokens(tok_func_name, except_token_tag<tok_oppar>{})) {
    if (vk::any_of_equal(tokens[tokens.size() - 2].str_val, "exit", "die")) {
      Token t = tokens.back();
      tokens.pop_back();
      tokens.emplace_back(tok_oppar);
      tokens.emplace_back(tok_clpar);
      tokens.push_back(t);
      return;
    }
  }


  if (are_last_tokens(except_token_tag<tok_function>{}, tok_func_name, tok_oppar, any_token_tag{})) {
    if (tokens[tokens.size() - 3].str_val == "err") {
      Token t = tokens.back();
      tokens.pop_back();
      tokens.emplace_back(tok_file_c);
      tokens.emplace_back(tok_comma);
      tokens.emplace_back(tok_line_c);
      if (t.type() != tok_clpar) {
        tokens.emplace_back(tok_comma);
      }
      tokens.push_back(t);
      return;
    }
  }

  /**
   * Для случаев:
   *   \VK\Foo::array
   *   \VK\Foo::try
   *   \VK\Foo::$static_field
   * после tok_double_colon будет tok_array или tok_try, а мы хотим tok_func_name
   * так как это корректные имена переменных
   * поэтому проверяем является ли первый символ следующего токена is_alpha, чтобы не пропустить tok_opbrk и тому подобное
   */
  if (are_last_tokens(tok_static, tok_double_colon, any_token_tag{}) || are_last_tokens(tok_func_name, tok_double_colon, any_token_tag{})) {
    if (!tokens.back().str_val.empty() && is_alpha(tokens.back().str_val[0])) {
      string val = static_cast<std::string>(tokens[tokens.size() - 3].str_val);
      val += "::";
      val += static_cast<std::string>(tokens[tokens.size() - 1].str_val);
      Token back = tokens.back();
      remove_last_tokens(3);
      tokens.emplace_back(back.type() == tok_var_name ? tok_var_name : tok_func_name, string_view_dup(val));
      tokens.back().line_num = back.line_num;
      return;
    }
  }

  /**
   * Хак для того, чтобы функции с такиим именами парсились в functions.txt,
   * но при этом это было отдельными токенами, т.к. их надо парсить чуть по другому
   */
  if (are_last_tokens(tok_function, tok_var_dump) || are_last_tokens(tok_function, tok_dbg_echo) || are_last_tokens(tok_function, tok_print) || are_last_tokens(tok_function, tok_echo)) {
    tokens.back() = {tok_func_name, tokens.back().str_val};
  }

  /**
   * Для случаев, когда встречаются ключевые слова после ->, const, это должны быть tok_func_name,
   * а не tok_array, tok_try и т.д.
   * например:
   *     $c->array, $c->try
   *     class U { const array = [1, 2]; }
   *     class U { const try = [1, 2]; }
   */
  if (are_last_tokens(tok_const, any_token_tag{}) || are_last_tokens(tok_arrow, any_token_tag{})) {
    if (!tokens.back().str_val.empty() && is_alpha(tokens.back().str_val[0])) {
      tokens.back().type_ = tok_func_name;
      return;
    }
  }
}

void LexerData::set_dont_hack_last_tokens() {
  dont_hack_last_tokens = true;
}

std::vector<Token> &&LexerData::move_tokens() {
  return std::move(tokens);
}

int LexerData::get_line_num() {
  return line_num;
}


int parse_with_helper(LexerData *lexer_data, const std::unique_ptr<Helper<TokenLexer>> &h) {
  const char *s = lexer_data->get_code();

  TokenLexer *fnd = h->get_help(s);

  int ret;
  if (fnd == nullptr || (ret = fnd->parse(lexer_data)) != 0) {
    ret = h->get_default()->parse(lexer_data);
  }

  return ret;
}

int TokenLexerError::parse(LexerData *lexer_data) const {
  stage::set_line(lexer_data->get_line_num());
  kphp_error (0, error_str.c_str());
  return 1;
}

int TokenLexerName::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code(), *st = s;
  const TokenType type = s[0] == '$' ? tok_var_name : tok_func_name;

  if (type == tok_var_name) {
    ++s;
  }

  const char *t = s;
  if (type == tok_var_name) {
    if (t[0] == '{') {
      return TokenLexerError("${ is not supported by kPHP").parse(lexer_data);
    }
    if (is_alpha(t[0])) {
      t++;
      while (is_alphanum(t[0])) {
        t++;
      }
    }
  } else {
    if (is_alpha(t[0]) || t[0] == '\\') {
      t++;
      while (is_alphanum(t[0]) || t[0] == '\\') {
        t++;
      }
    }
    if (s != t) {
      bool bad = false;
      if (t[-1] == '\\') {
        bad = true;
      }
      for (const char *cur = s; cur + 1 != t; cur++) {
        if (cur[0] == '\\' && cur[1] == '\\') {
          bad = true;
          break;
        }
      }
      if (bad) {
        return TokenLexerError("Bad function name " + string(s, t)).parse(lexer_data);
      }
    }
  }

  if (s == t) {
    return TokenLexerError("Variable name expected").parse(lexer_data);
  }

  vk::string_view name(s, t);

  if (type == tok_func_name) {
    const KeywordType *tp = KeywordsSet::get_type(name.begin(), name.size());
    if (tp != nullptr) {
      lexer_data->add_token((int)(t - st), tp->type, s, t);
      return 0;
    }
  } else if (name == "GLOBALS") {
    return TokenLexerError("$GLOBALS is not supported").parse(lexer_data);
  }

  lexer_data->add_token((int)(t - st), type, name);
  return 0;
}

int TokenLexerNum::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code();

  const char *t = s;

  enum {
    before_dot,
    after_dot,
    after_e,
    after_e_and_sign,
    after_e_and_digit,
    finish,
    hex,
    binary,
  } state = before_dot;

  if (s[0] == '0' && s[1] == 'x') {
    t += 2;
    state = hex;
  } else if (s[0] == '0' && s[1] == 'b') {
    t += 2;
    state = binary;
  }

  bool is_float = false;

  while (*t && state != finish) {
    switch (state) {
      case hex: {
        switch (*t) {
          case '0' ... '9':
          case 'A' ... 'F':
          case 'a' ... 'f':
            t++;
            break;
          default:
            state = finish;
            break;
        }
        break;
      }
      case binary: {
        switch(*t) {
          case '0':
          case '1':
            t++;
            break;
          default:
            state = finish;
            break;
        }
        break;
      }
      case before_dot: {
        switch (*t) {
          case '0' ... '9': {
            t++;
            break;
          }
          case '.': {
            t++;
            is_float = true;
            state = after_dot;
            break;
          }
          case 'e':
          case 'E': {
            t++;
            is_float = true;
            state = after_e;
            break;
          }
          default: {
            state = finish;
            break;
          }
        }
        break;
      }
      case after_dot: {
        switch (*t) {
          case '0' ... '9': {
            t++;
            break;
          }
          case 'e':
          case 'E': {
            t++;
            state = after_e;
            break;
          }
          default: {
            state = finish;
            break;
          }
        }
        break;
      }
      case after_e: {
        switch (*t) {
          case '-':
          case '+': {
            t++;
            state = after_e_and_sign;
            break;
          }
          case '0' ... '9': {
            t++;
            state = after_e_and_digit;
            break;
          }
          default: {
            return TokenLexerError("Bad exponent").parse(lexer_data);
          }
        }
        break;
      }
      case after_e_and_sign: {
        switch (*t) {
          case '0' ... '9': {
            t++;
            state = after_e_and_digit;
            break;
          }
          default: {
            return TokenLexerError("Bad exponent").parse(lexer_data);
          }
        }
        break;
      }
      case after_e_and_digit: {
        switch (*t) {
          case '0' ... '9': {
            t++;
            break;
          }
          default: {
            state = finish;
            break;
          }
        }
        break;
      }

      case finish: {
        assert (0);
      }
    }
  }

  if (!is_float) {
    if (s[0] == '0' && s[1] != 'x' && s[1] != 'b') {
      for (int i = 0; i < t - s; i++) {
        if (s[i] < '0' || s[i] > '7') {
          return TokenLexerError("Bad octal number").parse(lexer_data);
        }
      }
    }
  }

  assert (t != s);
  lexer_data->add_token((int)(t - s), is_float ? tok_float_const : tok_int_const, s, t);

  return 0;
}


int TokenLexerSimpleString::parse(LexerData *lexer_data) const {
  string str;

  const char *s = lexer_data->get_code();
  const char *t = s + 1;

  lexer_data->pass_raw(1);
  bool need = true;
  lexer_data->start_str();
  while (need) {
    switch (t[0]) {
      case 0:
        return TokenLexerError("Unexpected end of file").parse(lexer_data);
        continue;

      case '\'':
        need = false;
        t++;
        continue;

      case '\\':
        if (t[1] == '\\') {
          t += 2;
          lexer_data->append_char('\\');
          lexer_data->pass_raw(2);
          continue;
        } else if (t[1] == '\'') {
          t += 2;
          lexer_data->append_char('\'');
          lexer_data->pass_raw(2);
          continue;
        }
        break;

      default:
        break;
    }
    lexer_data->append_char(-1);
    t++;
  }
  lexer_data->flush_str();
  lexer_data->pass_raw(1);
  return 0;
}

TokenLexerAppendChar::TokenLexerAppendChar(int c, int pass) :
  c(c),
  pass(pass) {
}

int TokenLexerAppendChar::parse(LexerData *lexer_data) const {
  lexer_data->append_char(c);
  lexer_data->pass_raw(pass);
  return 0;
}

int TokenLexerOctChar::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code(), *t = s;
  int val = t[1] - '0';
  t += 2;

  int add = conv_oct_digit(*t);
  if (add != -1) {
    val = (val << 3) + add;

    add = conv_oct_digit(*++t);
    if (add != -1) {
      val = (val << 3) + add;
      t++;
    }
  }

  //TODO: \777
  lexer_data->append_char(val);
  lexer_data->pass_raw((int)(t - s));
  return 0;
}

int TokenLexerHexChar::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code(), *t = s + 2;
  int val = conv_hex_digit(*t);
  if (val == -1) {
    return TokenLexerError("It is not hex char").parse(lexer_data);
  }

  int add = conv_hex_digit(*++t);
  if (add != -1) {
    val = (val << 4) + add;
    t++;
  }

  lexer_data->append_char(val);
  lexer_data->pass_raw((int)(t - s));
  return 0;
}


void TokenLexerStringExpr::init() {
  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerError("Can't parse"));

  h->add_simple_rule("\'", &vk::singleton<TokenLexerSimpleString>::get());
  h->add_simple_rule("\"", &vk::singleton<TokenLexerString>::get());
  h->add_rule("[a-zA-Z_$\\]", &vk::singleton<TokenLexerName>::get());

  //TODO: double (?)
  h->add_rule("[0-9]|.[0-9]", &vk::singleton<TokenLexerNum>::get());

  h->add_rule(" |\t|\n|\r", &vk::singleton<TokenLexerSkip>::get());
  h->add_simple_rule("", &vk::singleton<TokenLexerCommon>::get());
}

int TokenLexerStringExpr::parse(LexerData *lexer_data) const {
  assert (h != nullptr);
  const char *s = lexer_data->get_code();
  assert (*s == '{');
  lexer_data->add_token(1, tok_expr_begin);

  int bal = 0;
  while (true) {
    const char *s = lexer_data->get_code();

    if (*s == 0) {
      return TokenLexerError("Unexpected end of file").parse(lexer_data);
    } else if (*s == '{') {
      bal++;
    } else if (*s == '}') {
      if (bal == 0) {
        lexer_data->add_token(1, tok_expr_end);
        break;
      }
      bal--;
    }

    int res = parse_with_helper(lexer_data, h);
    if (res) {
      return res;
    }
  }
  return 0;
}


void TokenLexerString::add_esc(const string &s, char c) {
  h->add_simple_rule(s, new TokenLexerAppendChar(c, (int)s.size()));
}

void TokenLexerHeredocString::add_esc(const string &s, char c) {
  h->add_simple_rule(s, new TokenLexerAppendChar(c, (int)s.size()));
}

void TokenLexerString::init() {
  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerAppendChar(-1, 0));

  add_esc("\\f", '\f');
  add_esc("\\n", '\n');
  add_esc("\\r", '\r');
  add_esc("\\t", '\t');
  add_esc("\\v", '\v');
  add_esc("\\$", '$');
  add_esc("\\\\", '\\');
  add_esc("\\\"", '\"');

  h->add_rule("\\[0-7]", &vk::singleton<TokenLexerOctChar>::get());
  h->add_rule("\\x[0-9A-Fa-f]", &vk::singleton<TokenLexerHexChar>::get());

  h->add_rule("$[A-Za-z_{]", &vk::singleton<TokenLexerName>::get());
  h->add_simple_rule("{$", &vk::singleton<TokenLexerStringExpr>::get());
}

void TokenLexerHeredocString::init() {
  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerAppendChar(-1, 0));

  add_esc("\\f", '\f');
  add_esc("\\n", '\n');
  add_esc("\\r", '\r');
  add_esc("\\t", '\t');
  add_esc("\\v", '\v');
  add_esc("\\$", '$');
  add_esc("\\\\", '\\');

  h->add_rule("\\[0-7]", &vk::singleton<TokenLexerOctChar>::get());
  h->add_rule("\\x[0-9A-Fa-f]", &vk::singleton<TokenLexerHexChar>::get());

  h->add_rule("$[A-Za-z{]", &vk::singleton<TokenLexerName>::get());
  h->add_simple_rule("{$", &vk::singleton<TokenLexerStringExpr>::get());
}

int TokenLexerString::parse(LexerData *lexer_data) const {
  assert (h != nullptr);
  const char *s = lexer_data->get_code();
  int is_heredoc = s[0] == '<';
  assert (!is_heredoc);

  lexer_data->add_token(1, tok_str_begin);

  while (true) {
    const char *s = lexer_data->get_code();
    if (*s == '\"') {
      lexer_data->add_token(1, tok_str_end);
      break;
    }

    if (*s == 0) {
      return TokenLexerError("Unexpected end of file").parse(lexer_data);
    }

    int res = parse_with_helper(lexer_data, h);
    if (res) {
      return res;
    }
  }
  return 0;
}

int TokenLexerHeredocString::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code();
  int is_heredoc = s[0] == '<';
  assert (is_heredoc);

  string tag;
  const char *st = s;
  assert (s[1] == '<' && s[2] == '<');

  s += 3;

  while (s[0] == ' ') {
    s++;
  }

  bool double_quote = s[0] == '"';
  bool single_quote = s[0] == '\'';

  if (double_quote || single_quote) {
    s++;
  }

  while (is_alpha(s[0])) {
    tag += *s++;
  }

  if (tag.empty()) {
    return TokenLexerError("TAG expected").parse(lexer_data);
  }
  if (double_quote && s[0] != '"') {
    return TokenLexerError("\" expected").parse(lexer_data);
  }
  if (single_quote && s[0] != '\'') {
    return TokenLexerError("' expected").parse(lexer_data);
  }
  if (double_quote || single_quote) {
    s++;
  }
  if (s[0] != '\n') {
    return TokenLexerError("'\\n' expected").parse(lexer_data);
  }
  s++;

  if (!single_quote) {
    lexer_data->add_token((int)(s - st), tok_str_begin);
    assert (s == lexer_data->get_code());
  } else {
    lexer_data->start_str();
    lexer_data->pass_raw((int)(s - st));
    assert (s == lexer_data->get_code());
  }
  bool first = true;
  while (true) {
    const char *s = lexer_data->get_code();
    const char *t = s, *st = s;
    if (t[0] == '\n' || first) {
      t += t[0] == '\n';
      if (!strncmp(t, tag.c_str(), tag.size())) {
        t += tag.size();

        int semicolon = 0;
        if (t[0] == ';') {
          t++;
          semicolon = 1;
        }
        if (t[0] == '\n' || t[0] == 0) {
          if (!single_quote) {
            lexer_data->add_token((int)(t - st - semicolon), tok_str_end);
          } else {
            lexer_data->flush_str();
            lexer_data->pass_raw((int)(t - st - semicolon));;
          }
          break;
        }
      }
    }

    if (*s == 0) {
      return TokenLexerError("Unexpected end of file").parse(lexer_data);
    }

    if (!single_quote) {
      int res = parse_with_helper(lexer_data, h);
      if (res) {
        return res;
      }
    } else {
      lexer_data->append_char(-1);
    }
    first = false;
  }
  return 0;
}

int TokenLexerComment::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code(),
    *st = s;

  assert (s[0] == '/' || s[0] == '#');
  if (s[0] == '#' || s[1] == '/') {
    while (s[0] && s[0] != '\n') {
      s++;
    }
  } else {
    s += 2;
    if (s[0] && s[1] && s[0] == '*' && s[1] != '/') { // phpdoc
      char const *phpdoc_start = s;
      bool is_phpdoc = false;
      while (s[0] && (s[0] != '*' || s[1] != '/')) {
        // @return, @var, @param, @type, etc
        if (s[0] == '@') {
          is_phpdoc = true;
        }
        s++;
      }
      if (is_phpdoc) {
        lexer_data->add_token(0, tok_phpdoc, phpdoc_start, s);
      }
    } else {
      while (s[0] && (s[0] != '*' || s[1] != '/')) {
        s++;
      }
    }
    if (s[0]) {
      s += 2;
    } else {
      return TokenLexerError("Unclosed comment (*/ expected)").parse(lexer_data);
    }
  }

  lexer_data->pass((int)(s - st));
  return 0;
}

int TokenLexerIfndefComment::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code(),
    *st = s;

  assert (strncmp(s, "#ifndef KittenPHP", 17) == 0);
  s += 17;
  // ищем \n\s*#endif
  while (*s) {
    if (*s != '\n') {
      s++;
      continue;
    }
    for (++s; *s == ' ' || *s == '\t'; ++s) {
    }
    if (!strncmp(s, "#endif", 6)) {
      break;
    }
  }
  if (*s) {
    s += 6;
  } else {
    return TokenLexerError("Unclosed comment (#endif expected)").parse(lexer_data);
  }
  lexer_data->pass((int)(s - st));
  return 0;
}

int TokenLexerWithHelper::parse(LexerData *lexer_data) const {
  assert (h != nullptr);
  return parse_with_helper(lexer_data, h);
}

int TokenLexerToken::parse(LexerData *lexer_data) const {
  lexer_data->add_token(len, tp);
  return 0;
}

void TokenLexerCommon::add_rule(const std::unique_ptr<Helper<TokenLexer>> &h, const string &str, TokenType tp) {
  h->add_simple_rule(str, new TokenLexerToken(tp, (int)str.size()));
}

void TokenLexerCommon::init() {
  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerError("No <common token> found"));

  add_rule(h, ":::", tok_triple_colon);
  add_rule(h, ":<=:", tok_triple_lt);
  add_rule(h, ":>=:", tok_triple_gt);

  add_rule(h, "=", tok_eq1);
  add_rule(h, "==", tok_eq2);
  add_rule(h, "===", tok_eq3);
  add_rule(h, "<>", tok_neq_lg);
  add_rule(h, "!=", tok_neq2);
  add_rule(h, "!==", tok_neq3);
  add_rule(h, "<=>", tok_spaceship);
  add_rule(h, "<", tok_lt);
  add_rule(h, "<=", tok_le);
  add_rule(h, ">", tok_gt);
  add_rule(h, ">=", tok_ge);

  add_rule(h, "(", tok_oppar);
  add_rule(h, ")", tok_clpar);
  add_rule(h, "[", tok_opbrk);
  add_rule(h, "]", tok_clbrk);
  add_rule(h, "{", tok_opbrc);
  add_rule(h, "}", tok_clbrc);
  add_rule(h, ":", tok_colon);
  add_rule(h, ";", tok_semicolon);
  add_rule(h, ".", tok_dot);
  add_rule(h, ",", tok_comma);

  add_rule(h, "**", tok_pow);
  add_rule(h, "++", tok_inc);
  add_rule(h, "--", tok_dec);
  add_rule(h, "+", tok_plus);
  add_rule(h, "-", tok_minus);
  add_rule(h, "*", tok_times);
  add_rule(h, "/", tok_divide);

  add_rule(h, "@", tok_at);

  add_rule(h, "%", tok_mod);
  add_rule(h, "&", tok_and);
  add_rule(h, "|", tok_or);
  add_rule(h, "^", tok_xor);
  add_rule(h, "~", tok_not);
  add_rule(h, "!", tok_log_not);
  add_rule(h, "?", tok_question);
  add_rule(h, "??", tok_null_coalesce);

  add_rule(h, "<<", tok_shl);
  add_rule(h, ">>", tok_shr);
  add_rule(h, "+=", tok_set_add);
  add_rule(h, "-=", tok_set_sub);
  add_rule(h, "*=", tok_set_mul);
  add_rule(h, "/=", tok_set_div);
  add_rule(h, "%=", tok_set_mod);
  add_rule(h, "**=", tok_set_pow);
  add_rule(h, "&=", tok_set_and);
  add_rule(h, "&&", tok_log_and);
  add_rule(h, "|=", tok_set_or);
  add_rule(h, "||", tok_log_or);
  add_rule(h, "^=", tok_set_xor);
  add_rule(h, ".=", tok_set_dot);
  add_rule(h, ">>=", tok_set_shr);
  add_rule(h, "<<=", tok_set_shl);

  add_rule(h, "=>", tok_double_arrow);
  add_rule(h, "::", tok_double_colon);
  add_rule(h, "->", tok_arrow);
  add_rule(h, "...", tok_varg);
}

int TokenLexerSkip::parse(LexerData *lexer_data) const {
  lexer_data->pass(n);
  return 0;
}

void TokenLexerPHP::init() {
  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerError("Can't parse"));

  h->add_rule("/*|//|#", &vk::singleton<TokenLexerComment>::get());
  h->add_simple_rule("#ifndef KittenPHP", &vk::singleton<TokenLexerIfndefComment>::get());
  h->add_simple_rule("\'", &vk::singleton<TokenLexerSimpleString>::get());
  h->add_simple_rule("\"", &vk::singleton<TokenLexerString>::get());
  h->add_simple_rule("<<<", &vk::singleton<TokenLexerHeredocString>::get());
  h->add_rule("[a-zA-Z_$\\]", &vk::singleton<TokenLexerName>::get());

  h->add_rule("[0-9]|.[0-9]", &vk::singleton<TokenLexerNum>::get());

  h->add_rule(" |\t|\n|\r", &vk::singleton<TokenLexerSkip>::get());
  h->add_simple_rule("", &vk::singleton<TokenLexerCommon>::get());
}

void TokenLexerPHPDoc::add_rule(const std::unique_ptr<Helper<TokenLexer>> &h, const string &str, TokenType tp) {
  h->add_simple_rule(str, new TokenLexerToken(tp, (int)str.size()));
}

void TokenLexerPHPDoc::init() {
  struct TokenLexerPHPDocStopParsing : TokenLexer {
    int parse(LexerData *lexer_data) const {
      lexer_data->add_token(0, tok_end);
      lexer_data->pass(1);
      return 1;
    }
  };

  assert(!h);
  h = std::make_unique<Helper<TokenLexer>>(new TokenLexerPHPDocStopParsing());

  h->add_rule("[a-zA-Z_$\\]", &vk::singleton<TokenLexerName>::get());
  h->add_rule("[0-9]|.[0-9]", &vk::singleton<TokenLexerNum>::get());
  h->add_rule(" |\t|\n|\r", &vk::singleton<TokenLexerSkip>::get());

  add_rule(h, "<", tok_lt);
  add_rule(h, ">", tok_gt);
  add_rule(h, "(", tok_oppar);
  add_rule(h, ")", tok_clpar);
  add_rule(h, "[", tok_opbrk);
  add_rule(h, "]", tok_clbrk);
  add_rule(h, "{", tok_opbrc);
  add_rule(h, "}", tok_clbrc);
  add_rule(h, ":", tok_colon);
  add_rule(h, ";", tok_semicolon);
  add_rule(h, ".", tok_dot);
  add_rule(h, ",", tok_comma);
  add_rule(h, "-", tok_minus);
  add_rule(h, "@", tok_at);
  add_rule(h, "&", tok_and);
  add_rule(h, "*", tok_times);
  add_rule(h, "|", tok_or);
  add_rule(h, "^", tok_xor);
  add_rule(h, "!", tok_log_not);
  add_rule(h, "?", tok_question);
  add_rule(h, "::", tok_double_colon);
  add_rule(h, "=>", tok_double_arrow);
  add_rule(h, "->", tok_arrow);
  add_rule(h, "...", tok_varg);
}

int TokenLexerGlobal::parse(LexerData *lexer_data) const {
  const char *s = lexer_data->get_code();
  const char *t = s;
  while (*t && strncmp(t, "<?", 2)) {
    t++;
  }

  if (s != t) {
    lexer_data->add_token((int)(t - s), tok_inline_html, s, t);
    return 0;
  }

  if (*s == 0) {
    return TokenLexerError("End of file").parse(lexer_data);
  }

  if (!strncmp(s + 2, "php", 3)) {
    lexer_data->pass_raw(strlen("<?php"));
  } else {
    lexer_data->pass_raw(strlen("<?"));
  }


  while (true) {
    const char *s = lexer_data->get_code();
    const char *t = s;
    while (t[0] == ' ' || t[0] == '\t') {
      t++;
    }
    lexer_data->pass_raw((int)(t - s));
    s = t;
    if (s[0] == 0 || (s[0] == '?' && s[1] == '>')) {
      break;
    }
    int ret = php_lexer->parse(lexer_data);
    if (ret != 0) {
      return ret;
    }
  }
  lexer_data->add_token(0, tok_semicolon);
  if (*lexer_data->get_code()) {
    lexer_data->pass(2);
  }
  if (*lexer_data->get_code() == '\n') {
    lexer_data->pass(1);
  } else {
    while (*lexer_data->get_code() == ' ') {
      lexer_data->pass(1);
    }
  }

  return 0;
}

void lexer_init() {
  vk::singleton<TokenLexerCommon>::get().init();
  vk::singleton<TokenLexerStringExpr>::get().init();
  vk::singleton<TokenLexerString>::get().init();
  vk::singleton<TokenLexerHeredocString>::get().init();
  vk::singleton<TokenLexerPHP>::get().init();
  vk::singleton<TokenLexerPHPDoc>::get().init();
}

vector<Token> php_text_to_tokens(vk::string_view text) {
  static TokenLexerGlobal lexer;

  LexerData lexer_data{text};

  while (*lexer_data.get_code()) {
    if (lexer.parse(&lexer_data) != 0) {
      kphp_error(false, "failed to parse");
      return {};
    }
  }

  auto tokens = lexer_data.move_tokens();
  tokens.emplace_back(tok_end);
  return tokens;
}

vector<Token> phpdoc_to_tokens(vk::string_view text) {
  LexerData lexer_data{text};
  lexer_data.set_dont_hack_last_tokens();   // future(int) — не нужно (int) как op_conv_int

  while (*lexer_data.get_code()) {
    if (vk::singleton<TokenLexerPHPDoc>::get().parse(&lexer_data) == 1) {
      break;
    }

    // обычный паттерн для phpdoc переменной — это "some_type|(or | complex) $var any comment ..."
    // т.е. $var внутри самого phpdoc-типа не встречается, и когда встретили — стоп, потому что зачем дальше
    // исключение — когда "$var some_type|(or | complex) any comment ..." (то токенайзим всё)
    if (lexer_data.are_last_tokens(tok_var_name) && lexer_data.get_num_tokens() > 1) {
      break;
    }
  }

  if (!lexer_data.are_last_tokens(tok_end)) {
    lexer_data.add_token(0, tok_end);
  }
  return lexer_data.move_tokens();
}

