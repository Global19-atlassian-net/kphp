#include "compiler/tl-classes.h"

#include "compiler/stage.h"
#include "common/wrappers/fmt_format.h"
#include "compiler/data/class-data.h"
#include "common/tlo-parsing/replace-anonymous-args.h"
#include "common/tlo-parsing/flat-optimization.h"


void TlClasses::load_from(const std::string &tlo_schema) {
  auto tl_expected_ptr = vk::tl::parse_tlo(tlo_schema.c_str(), true);
  kphp_error_return(tl_expected_ptr.has_value(),
                    fmt_format("Error while reading tlo: {}", tl_expected_ptr.error()));

  std::unique_ptr<vk::tl::tl_scheme> scheme = std::move(std::move(tl_expected_ptr).value());
  std::string error;
  if (!vk::tl::replace_anonymous_args(*scheme, error)) {
    kphp_error_return(false, error.c_str());
  }
  if (!vk::tl::perform_flat_optimization(*scheme, error)) {
    kphp_error_return(false, error.c_str());
  }

  scheme_ = std::move(scheme);
  php_classes_.load_from(*scheme_);
}

