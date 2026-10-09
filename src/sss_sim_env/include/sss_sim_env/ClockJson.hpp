#pragma once
#include <json/json.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace sss_utils::clock_json {
using Object = std::map<std::string, Json::Value>;
inline Json::Value object(const Object& fields) {
  Json::Value result(Json::objectValue);
  for (const auto& field : fields) result[field.first] = field.second;
  return result;
}
inline Json::Value parse(const std::string& text) {
  Json::CharReaderBuilder builder;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  Json::Value value;
  std::string error;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  if (!reader->parse(text.data(), text.data() + text.size(), &value, &error))
    throw std::invalid_argument("invalid JSON");
  return value;
}
inline std::string serialize(const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
inline std::string serialize(const Object& fields) { return serialize(object(fields)); }
}
