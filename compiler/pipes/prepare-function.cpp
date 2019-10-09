#include "compiler/pipes/prepare-function.h"

#include <sstream>
#include <unordered_map>

#include "compiler/compiler-core.h"
#include "compiler/data/class-data.h"
#include "compiler/data/function-data.h"
#include "compiler/data/lambda-class-data.h"
#include "compiler/phpdoc.h"
#include "compiler/utils/string-utils.h"
#include "compiler/vertex.h"

//static void check_template_function(FunctionPtr func) {
//  if (!func->is_template) return;
//  for (auto l : func->lambdas_inside) {
//    const auto &prev_location = stage::get_location();
//    stage::set_location(l->class_id->construct_function->root->location);
//    kphp_error(!l->is_lambda_with_uses(), "it's not allowed lambda with uses inside template function(or another lambda)");
//    stage::set_location(prev_location);
//  }
//}

/*
 * Анализ @kphp-infer, @kphp-inline и других @kphp-* внутри phpdoc над функцией f
 * Сейчас пока что есть костыль: @kphp-required анализируется всё равно в gentree
 */
static void parse_and_apply_function_kphp_phpdoc(FunctionPtr f) {
  bool function_has_kphp_doc = f->phpdoc_str.find("@kphp") != std::string::npos;
  bool class_has_kphp_doc = (f->modifiers.is_instance() || f->modifiers.is_static()) && f->class_id->phpdoc_str.find("@kphp") != std::string::npos;
  if (!function_has_kphp_doc && !class_has_kphp_doc) {
    return;   // обычный phpdoc, без @kphp нотаций, тут не парсим; если там инстансы, распарсится по требованию
  }

  using infer_mask = FunctionData::InferHint::infer_mask;

  int infer_type = 0;
  VertexRange func_params = f->get_params();
  const vector<php_doc_tag> &tags = parse_php_doc(f->phpdoc_str);

  std::unordered_map<std::string, int> name_to_function_param;
  int param_i = 0;
  for (auto param : func_params) {
    name_to_function_param.emplace(param.as<meta_op_func_param>()->var()->get_string(), param_i++);
  }

  // phpdoc класса может влиять на phpdoc функции
  if (class_has_kphp_doc) {
    for (auto &tag : parse_php_doc(f->class_id->phpdoc_str)) {
      switch (tag.type) {
        // @kphp-infer, написанный над классом — будто его написали над каждой функцией
        case php_doc_tag::kphp_infer: {
          infer_type |= (infer_mask::check | infer_mask::hint);
          break;
        }

        default:
          break;
      }
    }
  }

  std::size_t id_of_kphp_template = 0;
  stage::set_location(f->root->get_location());
  for (auto &tag : tags) {
    stage::set_line(tag.line_num);
    switch (tag.type) {
      case php_doc_tag::kphp_inline: {
        f->is_inline = true;
        continue;
      }

      case php_doc_tag::kphp_sync: {
        f->should_be_sync = true;
        continue;
      }

      case php_doc_tag::kphp_infer: {
        infer_type |= (infer_mask::check | infer_mask::hint);
        if (tag.value.find("cast") != std::string::npos) {
          infer_type |= infer_mask::cast;
        }
        break;
      }

      case php_doc_tag::kphp_disable_warnings: {
        std::istringstream is(tag.value);
        std::string token;
        while (is >> token) {
          if (!f->disabled_warnings.insert(token).second) {
            kphp_warning(fmt_format("Warning '{}' has been disabled twice", token));
          }
        }
        break;
      }

      case php_doc_tag::kphp_extern_func_info: {
        kphp_error(f->is_extern(), "@kphp-extern-func-info used for regular function");
        std::istringstream is(tag.value);
        std::string token;
        while (is >> token) {
          if (token == "can_throw") {
            f->can_throw = true;
          } else if (token == "resumable") {
            f->is_resumable = true;
          } else if (token == "cpp_template_call") {
            f->cpp_template_call = true;
          } else if (token == "cpp_variadic_call") {
            f->cpp_variadic_call = true;
          } else if (token == "tl_common_h_dep") {
            f->tl_common_h_dep = true;
          } else {
            kphp_error(0, fmt_format("Unknown @kphp-extern-func-info {}", token));
          }
        }
        break;
      }

      case php_doc_tag::kphp_pure_function: {
        kphp_error(f->is_extern(), "@kphp-pure-function is supported only for built-in functions");
        if (f->root->type_rule) {
          f->root->type_rule->rule()->extra_type = op_ex_rule_const;
        }
        break;
      }

      case php_doc_tag::kphp_lib_export: {
        f->kphp_lib_export = true;
        infer_type |= (infer_mask::check | infer_mask::hint);
        break;
      }

      case php_doc_tag::kphp_template: {
        f->is_template = true;
        bool is_first_time = true;
        for (const auto &var_name : split_skipping_delimeters(tag.value, ", ")) {
          if (var_name[0] != '$') {
            if (is_first_time) {
              is_first_time = false;
              continue;
            }
            break;
          }
          is_first_time = false;

          auto func_param_it = name_to_function_param.find(var_name.substr(1));
          kphp_error_return(func_param_it != name_to_function_param.end(), fmt_format("@kphp-template tag var name mismatch. found {}.", var_name));

          auto cur_func_param = func_params[func_param_it->second].as<op_func_param>();
          name_to_function_param.erase(func_param_it);

          cur_func_param->template_type_id = id_of_kphp_template;
        }
        id_of_kphp_template++;

        break;
      }

      case php_doc_tag::kphp_const: {
        for (const auto &var_name : split_skipping_delimeters(tag.value, ", ")) {
          auto func_param_it = name_to_function_param.find(var_name.substr(1));
          kphp_error_return(func_param_it != name_to_function_param.end(), fmt_format("@kphp-const tag var name mismatch. found {}.", var_name));

          auto cur_func_param = func_params[func_param_it->second].as<op_func_param>();
          cur_func_param->var()->is_const = true;
        }

        break;
      }

      default:
        break;
    }
  }

  if (infer_type) {             // при наличии @kphp-infer парсим все @param'ы
    bool has_return_typehint = false;

    for (auto &tag : tags) {    // (вторым проходом, т.к. @kphp-infer может стоять в конце)
      stage::set_line(tag.line_num);
      switch (tag.type) {
        case php_doc_tag::returns: {
          PhpDocTagParseResult doc_parsed = phpdoc_parse_type_and_var_name(tag.value, f);
          if (!doc_parsed) {
            continue;
          }

          if (infer_type & infer_mask::check) {
            auto type_rule = VertexAdaptor<op_lt_type_rule>::create(doc_parsed.type_expr);
            f->add_kphp_infer_hint(infer_mask::check, -1, type_rule);
          }
          has_return_typehint = true;
          // hint для return'а не делаем совсем, чтобы не грубить вывод типов, только check
          break;
        }
        case php_doc_tag::param: {
          kphp_error_return(!name_to_function_param.empty(), "Too many @param tags");
          PhpDocTagParseResult doc_parsed = phpdoc_parse_type_and_var_name(tag.value, f);
          if (!doc_parsed) {
            continue;
          }

          auto func_param_it = name_to_function_param.find(doc_parsed.var_name);
          kphp_error_return(func_param_it != name_to_function_param.end(),
            fmt_format("@param tag var name mismatch: found ${}", doc_parsed.var_name));
          int param_i = func_param_it->second;

          auto cur_func_param = func_params[func_param_it->second].as<op_func_param>();
          name_to_function_param.erase(func_param_it);

          // если в phpdoc написано "callable" у этого @param
          if (doc_parsed.type_expr->type() == op_type_expr_callable) {
            f->is_template = true;
            cur_func_param->template_type_id = id_of_kphp_template;
            cur_func_param->is_callable = true;
            id_of_kphp_template++;
            continue;
          }

          if (infer_type & infer_mask::check) {
            auto type_rule = VertexAdaptor<op_lt_type_rule>::create(doc_parsed.type_expr);
            f->add_kphp_infer_hint(infer_mask::check, param_i, type_rule);
          }
          if (infer_type & infer_mask::hint) {
            auto type_rule = VertexAdaptor<op_common_type_rule>::create(doc_parsed.type_expr);
            f->add_kphp_infer_hint(infer_mask::hint, param_i, type_rule);
          }
          if (infer_type & infer_mask::cast) {
            kphp_error(doc_parsed.type_expr->type() == op_type_expr_type && doc_parsed.type_expr.as<op_type_expr_type>()->args().empty(),
                       "Too hard rule for cast");
            kphp_error(cur_func_param->type_help == tp_Unknown,
                       fmt_format("Duplicate type rule for argument '{}'", doc_parsed.var_name));
            cur_func_param->type_help = doc_parsed.type_expr.as<op_type_expr_type>()->type_help;
          }

          break;
        }
        default:
          break;
      }
    }

    // проверяем, что все @param заданы
    if (f->has_implicit_this_arg()) {
      name_to_function_param.erase(name_to_function_param.find("this"));
    }
    if (!name_to_function_param.empty()) {
      std::string err_msg = "Specify @param for arguments: ";
      for (const auto &name_and_function_param : name_to_function_param) {
        err_msg += "$" + name_and_function_param.first + " ";
      }
      stage::set_location(f->root->get_location());
      kphp_error(0, err_msg.c_str());
    }

    // если явного @return нет, то будто написано @return void
    if (!has_return_typehint && !f->is_constructor()) {
      auto parsed = phpdoc_parse_type_and_var_name("void", f);
      auto type_rule = VertexAdaptor<op_lt_type_rule>::create(parsed.type_expr);
      f->add_kphp_infer_hint(infer_mask::check, -1, type_rule);
    }
  }
}

static void set_template_flag_if_has_callable_arg(FunctionPtr fun) {
  auto params = fun->get_params();
  auto param_n = static_cast<int>(params.size());
  for (int i = 0; i < param_n; i++) {
    auto param = params[i].as<meta_op_func_param>();

    if (param->type_declaration == "callable") {
      param->is_callable = true;
      param->template_type_id = param_n + i;
      param->type_declaration.clear();
      fun->is_template = true;
    }
  }
}

static void check_default_args(FunctionPtr fun) {
  bool was_default = false;

  auto params = fun->get_params();
  for (size_t i = 0; i < params.size(); i++) {
    auto param = params[i].as<meta_op_func_param>();

    if (param->has_default_value() && param->default_value()) {
      was_default = true;
      if (fun->type == FunctionData::func_local) {
        kphp_error (!param->var()->ref_flag, fmt_format("Default value in reference function argument [function = {}]", fun->get_human_readable_name()));
      }
    } else {
      kphp_error (!was_default, fmt_format("Default value expected [function = {}] [param_i = {}]", fun->get_human_readable_name(), i));
    }
  }
}

static void apply_function_typehints(FunctionPtr function) {
  using infer_mask = FunctionData::InferHint::infer_mask;
  auto func_params = function->get_params();

  for (int i = 0; i < func_params.size(); ++i) {
    auto param = func_params[i].try_as<op_func_param>();
    if (param && param->type_declaration == "array") {
      auto parsed = phpdoc_parse_type_and_var_name(param->type_declaration, function);
      auto type_rule = VertexAdaptor<op_lt_type_rule>::create(parsed.type_expr);
      function->add_kphp_infer_hint(infer_mask::check, i, type_rule);
    }
  }
}

void PrepareFunctionF::execute(FunctionPtr function, DataStream<FunctionPtr> &os) {
  stage::set_name("Prepare function");
  stage::set_function(function);
  kphp_assert (function);

  parse_and_apply_function_kphp_phpdoc(function);
  set_template_flag_if_has_callable_arg(function);
  check_default_args(function);
  apply_function_typehints(function);

  auto is_function_name_allowed = [](vk::string_view name) {
    if (!name.starts_with("__")) {
      return true;
    }

    static std::string allowed_magic_names[]{ClassData::NAME_OF_CONSTRUCT,
                                             ClassData::NAME_OF_CLONE,
                                             FunctionData::get_name_of_self_method(ClassData::NAME_OF_CLONE),
                                             ClassData::NAME_OF_VIRT_CLONE,
                                             ClassData::NAME_OF_INVOKE_METHOD};

    return std::find(std::begin(allowed_magic_names), std::end(allowed_magic_names), name) != std::end(allowed_magic_names);
  };

  kphp_error(!function->class_id || is_function_name_allowed(function->local_name()),
             fmt_format("KPHP doesn't support magic method: {}", function->get_human_readable_name()));

  if (stage::has_error()) {
    return;
  }

  os << function;
}
