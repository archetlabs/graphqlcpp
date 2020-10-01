#pragma once
#ifndef GRAPHQL_FILE_HPP
#define GRAPHQL_FILE_HPP

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

namespace archet {
namespace graphql {
namespace loader {

std::vector<std::string> visual_load_file(std::string path) {
  std::vector<std::string> buffer;
  std::ifstream file(path);
  std::string line;
  while (getline(file, line)) {
    buffer.push_back(line);
  }
  return buffer;
}

std::vector<std::string> visual_load_string(std::string str) {
  std::vector<std::string> buffer;
  boost::split(buffer, str, boost::is_any_of("\n"));
  return buffer;
}

std::string visual_substr(std::vector<std::string> visual, int start_line, int start_column,
                          int end_line, int end_column) {
  start_line--;
  end_line--;
  start_column--;
  end_column--;

  if (start_line == end_line) {
    return visual[start_line].substr(start_column, end_column) + "\n";
  }

  std::string output =
      visual[start_line].substr(start_column, visual[start_line].length() - start_column) + "\n";

  for (int current_line = start_line + 1; current_line < end_line; current_line++) {
    output += visual[current_line] + "\n";
  }

  output += visual[end_line].substr(0, end_column);

  return output;
}

class definitions {
  std::unordered_map<std::string, std::string> definitions_;

public:
  void build_from_file(std::string gql_path, std::string ast_path) {
    std::vector<std::string> visual = visual_load_file(gql_path);
    std::ifstream ast_file(ast_path);
    nlohmann::json ast;
    ast_file >> ast;
    build(visual, ast);
  }

  void build_from_string(std::string gql_string, std::string ast_string) {
    std::vector<std::string> visual = visual_load_string(gql_string);
    nlohmann::json ast = nlohmann::json::parse(ast_string);
    build(visual, ast);
  }

  void build(std::vector<std::string> visual, nlohmann::json ast) {
    nlohmann::json definitions = ast["definitions"];
    for (nlohmann::json::iterator it = definitions.begin(); it != definitions.end(); ++it) {
      nlohmann::json definition = *it;
      std::string name = definition["name"]["value"];
      int start_line = definition["loc"]["start"]["line"].get<int>();
      int start_column = definition["loc"]["start"]["column"].get<int>();
      int end_line = definition["loc"]["end"]["line"].get<int>();
      int end_column = definition["loc"]["end"]["column"].get<int>();
      definitions_[name] = visual_substr(visual, start_line, start_column, end_line, end_column);
    }
  }

  std::string operator[](std::string name) { return definitions_[name]; }
};

definitions load_file(std::string gql_path, std::string ast_path) {
  definitions d;
  d.build_from_file(gql_path, ast_path);
  return d;
}

definitions load_string(std::string gql, std::string ast) {
  definitions d;
  d.build_from_string(gql, ast);
  return d;
}

definitions load_resource(unsigned char gql[], unsigned int gql_len, unsigned char ast[],
                          unsigned int ast_len) {
  std::string gql_str((char *)gql, gql_len);
  std::string ast_str((char *)ast, ast_len);
  definitions d;
  d.build_from_string(gql_str, ast_str);
  return d;
}

} // namespace loader
} // namespace graphql
} // namespace archet
#endif
