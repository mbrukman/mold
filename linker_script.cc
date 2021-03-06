// On Linux, /usr/lib/x86_64-linux-gnu/libc.so is not actually
// a shared object file but an ASCII text file containing a linker
// script to include a "real" libc.so file. Therefore, we need to
// support a (very limited) subset of the linker script language.

#include "mold.h"

static thread_local std::string script_dir;

static std::vector<std::string_view> tokenize(std::string_view input) {
  std::vector<std::string_view> vec;
  while (!input.empty()) {
    if (input[0] == ' ' || input[0] == '\t' || input[0] == '\n') {
      input = input.substr(1);
      continue;
    }

    if (input.starts_with("/*")) {
      i64 pos = input.find("*/", 2);
      if (pos == std::string_view::npos)
        Fatal() << "unclosed comment";
      input = input.substr(pos + 2);
      continue;
    }

    if (input[0] == '#') {
      i64 pos = input.find("\n", 1);
      if (pos == std::string_view::npos)
        break;
      input = input.substr(pos + 1);
      continue;
    }

    if (input[0] == '"') {
      i64 pos = input.find('"', 1);
      if (pos == std::string_view::npos)
        Fatal() << "unclosed string literal";
      vec.push_back(input.substr(0, pos));
      input = input.substr(pos);
      continue;
    }

    i64 pos = input.find_first_not_of(
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
      "0123456789_.$/\\~=+[]*?-!^:");

    if (pos == 0)
      pos = 1;
    vec.push_back(input.substr(0, pos));
    input = input.substr(pos);
  }
  return vec;
}

static std::span<std::string_view>
skip(std::span<std::string_view> tok, std::string_view str) {
  if (tok.empty() || tok[0] != str)
    Fatal() << "expected '" << str << "'";
  return tok.subspan(1);
}

static std::span<std::string_view> read_output_format(std::span<std::string_view> tok) {
  tok = skip(tok, "(");
  while (!tok.empty() && tok[0] != ")")
    tok = tok.subspan(1);
  if (tok.empty())
    Fatal() << "expected ')'";
  return tok.subspan(1);
}

static MemoryMappedFile *resolve_path(std::string str) {
  if (str.starts_with("/"))
    return MemoryMappedFile::must_open(config.sysroot + str);
  if (str.starts_with("-l"))
    return find_library(str.substr(2), config.library_paths);
  if (MemoryMappedFile *mb = MemoryMappedFile::open(script_dir + "/" + str))
    return mb;
  if (MemoryMappedFile *mb = MemoryMappedFile::open(str))
    return mb;
  for (std::string_view dir : config.library_paths) {
    std::string root = dir.starts_with("/") ? config.sysroot : "";
    if (MemoryMappedFile *mb = MemoryMappedFile::open(root + std::string(dir) + "/" + str))
      return mb;
  }
  Fatal() << "library not found: " << str;
}

static std::span<std::string_view>
read_group(std::span<std::string_view> tok, ReadContext &ctx) {
  tok = skip(tok, "(");

  while (!tok.empty() && tok[0] != ")") {
    if (tok[0] == "AS_NEEDED") {
      ReadContext ctx2 = ctx;
      ctx2.as_needed = true;
      tok = read_group(tok.subspan(1), ctx2);
      continue;
    }

    read_file(resolve_path(std::string(tok[0])), ctx);
    tok = tok.subspan(1);
  }

  if (tok.empty())
    Fatal() << "expected ')'";
  return tok.subspan(1);
}

void parse_linker_script(MemoryMappedFile *mb, ReadContext &ctx) {
  script_dir = mb->name.substr(0, mb->name.find_last_of('/'));

  std::vector<std::string_view> vec = tokenize(mb->get_contents());
  std::span<std::string_view> tok = vec;

  while (!tok.empty()) {
    if (tok[0] == "OUTPUT_FORMAT")
      tok = read_output_format(tok.subspan(1));
    else if (tok[0] == "INPUT" || tok[0] == "GROUP")
      tok = read_group(tok.subspan(1), ctx);
    else
      Fatal() << mb->name << ": unknown token: " << tok[0];
  }
}

void parse_version_script(std::string path) {
  script_dir = path.substr(0, path.find_last_of('/'));

  MemoryMappedFile *mb = MemoryMappedFile::must_open(path);
  std::vector<std::string_view> vec = tokenize(mb->get_contents());
  std::span<std::string_view> tok = vec;
  tok = skip(tok, "{");

  std::vector<std::string> locals;
  std::vector<std::string> globals;
  std::vector<std::string> *cur = &locals;

  while (!tok.empty() && tok[0] != "}") {
    if (tok[0] == "local:") {
      cur = &locals;
      tok = tok.subspan(1);
      continue;
    }

    if (tok.size() >= 2 && tok[0] == "local" && tok[1] == ":") {
      cur = &locals;
      tok = tok.subspan(2);
      continue;
    }

    if (tok[0] == "global:") {
      cur = &globals;
      tok = tok.subspan(1);
      continue;
    }

    if (tok.size() >= 2 && tok[0] == "global" && tok[1] == ":") {
      cur = &globals;
      tok = tok.subspan(2);
      continue;
    }

    cur->push_back(std::string(tok[0]));
    tok = skip(tok.subspan(1), ";");
  }

  tok = skip(tok, "}");
  tok = skip(tok, ";");

  if (!tok.empty())
    Fatal() << path << ": trailing garbage token: " << tok[0];

  if (locals.size() != 1 || locals[0] != "*")
    Fatal() << path << ": unsupported version script";
  config.export_dynamic = false;
  config.globals = globals;
}
