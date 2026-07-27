---
kind: external_dependency
name: JsonCpp JSON解析库
slug: jsoncpp
category: external_dependency
category_hints:
    - sdk_real_api
scope:
    - '**'
source_files:
    - server/*/src/LogicSystem.cpp
---

服务端统一使用 JsonCpp 进行 JSON 数据解析，头文件为 <json/json.h>、<json/value.h>、<json/reader.h>。与 nlohmann/json 不同，JsonCpp 使用 Json::Value、Json::Reader、Json::StreamWriter 等 API。客户端则使用 Qt 自带的 QJsonObject、QJsonArray、QJsonDocument。JsonCpp 在项目中的主要用途是 HTTP 请求响应数据的序列化和反序列化。