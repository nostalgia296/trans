#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <curl/curl.h>
#include <json/json.h>


struct Config {
    std::string base_url;
    std::string api_key;
    std::string model;
    int workers;
    int timeout_seconds;
};

// 默认配置值
const Config DEFAULT_CONFIG = {
    "https://api.deepseek.com/v1/chat/completions",
    "",
    "deepseek-chat",
    20,
    60
};


struct Job {
    std::string key;
    std::string text;
    int index;
};

struct Result {
    std::string key;
    std::string text;
    int index;
    std::string error;
};


class ConfigManager {
public:
    static Config loadConfig(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "警告: 无法打开配置文件 " << filename << "，使用默认配置" << std::endl;
            return DEFAULT_CONFIG;
        }
        
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errors;
        
        if (!Json::parseFromStream(reader, file, &root, &errors)) {
            std::cerr << "警告: 配置文件解析错误: " << errors << "，使用默认配置" << std::endl;
            return DEFAULT_CONFIG;
        }
        
        file.close();
        
        Config config;
        
        // 读取配置值，如果不存在则使用默认值
        config.base_url = root.get("base_url", DEFAULT_CONFIG.base_url).asString();
        config.api_key = root.get("api_key", DEFAULT_CONFIG.api_key).asString();
        config.model = root.get("model", DEFAULT_CONFIG.model).asString();  // 读取模型配置
        config.workers = root.get("workers", DEFAULT_CONFIG.workers).asInt();
        config.timeout_seconds = root.get("timeout_seconds", DEFAULT_CONFIG.timeout_seconds).asInt();
        
        // 验证必要的配置
        if (config.api_key.empty() || config.api_key == DEFAULT_CONFIG.api_key) {
            std::cerr << "警告: API密钥未配置或使用默认值，请检查config.json" << std::endl;
        }
        
        std::cout << "配置加载成功:" << std::endl;
        std::cout << "  Base URL: " << config.base_url << std::endl;
        std::cout << "  Model: " << config.model << std::endl;
        std::cout << "  Workers: " << config.workers << std::endl;
        std::cout << "  Timeout: " << config.timeout_seconds << "秒" << std::endl;
        
        return config;
    }
    
    static void createDefaultConfig(const std::string& filename) {
        Json::Value root;
        root["base_url"] = DEFAULT_CONFIG.base_url;
        root["api_key"] = "Your key";
        root["model"] = DEFAULT_CONFIG.model;
        root["workers"] = DEFAULT_CONFIG.workers;
        root["timeout_seconds"] = DEFAULT_CONFIG.timeout_seconds;
        
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::unique_ptr<Json::StreamWriter> jsonWriter(writer.newStreamWriter());
        
        std::ofstream file(filename);
        jsonWriter->write(root, &file);
        file.close();
        
        std::cout << "已创建默认配置文件: " << filename << std::endl;
        std::cout << "请编辑该文件并设置你的API密钥" << std::endl;
    }
};


size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t totalSize = size * nmemb;
    response->append((char*)contents, totalSize);
    return totalSize;
}


class HttpClient {
private:
    std::string base_url;
    std::string api_key;
    int timeout_seconds;
    
public:
    HttpClient(const Config& config) 
        : base_url(config.base_url), api_key(config.api_key), timeout_seconds(config.timeout_seconds) {}
    
    std::string post(const std::string& endpoint, const std::string& payload) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        std::string response;
        std::string url = base_url + endpoint;
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
        
        CURLcode res = curl_easy_perform(curl);
        
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL request failed: " + std::string(curl_easy_strerror(res)));
        }
        
        return response;
    }
};


class WorkerManager {
private:
    std::vector<Job> jobs;
    int concurrency;
    std::shared_ptr<HttpClient> client;
    std::atomic<int> current_index;
    std::mutex results_mutex;
    std::map<int, Result> results;
    std::string model;
    
    std::string escapeJsonString(const std::string& input) {
        std::string output;
        for (char c : input) {
            switch (c) {
                case '"':  output += "\\\""; break;
                case '\\': output += "\\\\"; break;
                case '\b': output += "\\b"; break;
                case '\f': output += "\\f"; break;
                case '\n': output += "\\n"; break;
                case '\r': output += "\\r"; break;
                case '\t': output += "\\t"; break;
                default:   output += c; break;
            }
        }
        return output;
    }
    
public:
    WorkerManager(const std::vector<Job>& jobs_list, const Config& config, std::shared_ptr<HttpClient> http_client)
        : jobs(jobs_list), concurrency(config.workers), client(http_client), current_index(0), model(config.model) {}  // 初始化模型
    
    std::string callAPI(const std::string& text) {
        std::string escaped_text = escapeJsonString(text);
        
        Json::Value payload;
        payload["model"] = model;
        
        Json::Value messages(Json::arrayValue);
        
        Json::Value system_msg;
        system_msg["role"] = "system";
        system_msg["content"] = "你是中日翻译大师，能够完美的把其中一种语言翻译成另一种";
        messages.append(system_msg);
        
        Json::Value user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = escaped_text;
        messages.append(user_msg);
        
        payload["messages"] = messages;
        
        Json::StreamWriterBuilder writer;
        std::string payload_str = Json::writeString(writer, payload);
        
        std::string response_str = client->post("", payload_str);
        
        Json::Value response_json;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream response_stream(response_str);
        
        if (!Json::parseFromStream(reader, response_stream, &response_json, &errors)) {
            throw std::runtime_error("Failed to parse JSON response: " + errors);
        }
        
        if (response_json.isMember("choices") && 
            response_json["choices"].isArray() && 
            !response_json["choices"].empty() &&
            response_json["choices"][0].isMember("message") &&
            response_json["choices"][0]["message"].isMember("content")) {
            
            return response_json["choices"][0]["message"]["content"].asString();
        }
        
        throw std::runtime_error("Invalid response format");
    }
    
    void worker() {
        while (true) {
            int idx = current_index.fetch_add(1);
            if (idx >= jobs.size()) {
                break;
            }
            
            const Job& job = jobs[idx];
            Result result;
            result.key = job.key;
            result.index = job.index;
            
            try {
                result.text = callAPI(job.text);
            } catch (const std::exception& e) {
                result.error = e.what();
            }
            
            std::lock_guard<std::mutex> lock(results_mutex);
            results[result.index] = result;
        }
    }
    
    std::vector<Result> run() {
        std::vector<std::thread> workers;
        
        for (int i = 0; i < concurrency; ++i) {
            workers.emplace_back(&WorkerManager::worker, this);
        }
        
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        
        std::vector<Result> sorted_results;
        for (const auto& pair : results) {
            sorted_results.push_back(pair.second);
        }
        
        return sorted_results;
    }
};

// 读取JSON文件
std::map<std::string, std::string> readJsonFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }
    
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    
    if (!Json::parseFromStream(reader, file, &root, &errors)) {
        throw std::runtime_error("Failed to parse JSON: " + errors);
    }
    
    file.close();
    
    std::map<std::string, std::string> result;
    for (auto it = root.begin(); it != root.end(); ++it) {
        result[it.key().asString()] = it->asString();
    }
    
    return result;
}

//避免Unicode转义
void writeJsonFile(const std::string& filename, const std::map<std::string, std::string>& data) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file for writing: " + filename);
    }
    
    file << "{" << std::endl;
    
    bool first = true;
    for (const auto& pair : data) {
        if (!first) {
            file << "," << std::endl;
        }
        first = false;
        
        // 键名（保持原样）
        file << "  \"" << pair.first << "\" : ";
        
        // 值 - 直接输出，不进行Unicode转义
        file << "\"" << pair.second << "\"";
    }
    
    file << std::endl << "}" << std::endl;
    file.close();
}

int main(int argc, char* argv[]) {
    // 检查是否要创建默认配置文件
    if (argc == 2 && std::string(argv[1]) == "--create-config") {
        ConfigManager::createDefaultConfig("config.json");
        return 0;
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    try {
        // 加载配置
        Config config = ConfigManager::loadConfig("config.json");
        
        // 读取原始数据
        auto orig = readJsonFile("trans.json");
        
        // 创建任务列表
        std::vector<Job> jobs;
        int index = 0;
        for (const auto& pair : orig) {
            jobs.push_back({pair.first, pair.second, index++});
        }
        
        // 创建HTTP客户端
        auto client = std::make_shared<HttpClient>(config);
        WorkerManager manager(jobs, config, client);
        auto results = manager.run();
        
        // 处理结果
        std::map<std::string, std::string> output;
        int success_count = 0;
        int fail_count = 0;
        
        for (const auto& result : results) {
            if (result.error.empty()) {
                output[result.key] = result.text;
                success_count++;
            } else {
                std::cerr << "key=" << result.key << " 翻译失败: " << result.error << std::endl;
                fail_count++;
            }
        }
        
        // 写入输出文件
        writeJsonFile("trans_output.json", output);
        
        std::cout << "全部翻译完成，结果已写入 trans_output.json" << std::endl;
        std::cout << "成功: " << success_count << ", 失败: " << fail_count << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        curl_global_cleanup();
        return 1;
    }
    
    curl_global_cleanup();
    return 0;
}
