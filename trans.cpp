#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <memory>
#include <sstream>
#include <random>
#include <chrono>
#include <iomanip>
#include <string>
#include <condition_variable>
#include <csignal>

#include <curl/curl.h>
#include <json/json.h>

std::string utf8_substr(const std::string& str, int num_chars) {
    if (num_chars <= 0) {
        return "";
    }

    int byte_index = 0;
    int char_count = 0;
    
    while (byte_index < str.length() && char_count < num_chars) {
        unsigned char c = str[byte_index];
        if (c < 0x80) {
            byte_index += 1;
        } else if ((c & 0xE0) == 0xC0) {
            byte_index += 2;
        } else if ((c & 0xF0) == 0xE0) {
            byte_index += 3;
        } else if ((c & 0xF8) == 0xF0) {
            byte_index += 4;
        } else {
            byte_index += 1;
        }
        char_count++;
    }

    if (byte_index > str.length()) {
        return str;
    }

    return str.substr(0, byte_index);
}

std::map<std::string, std::string> readJsonFile(const std::string& filename);
void writeJsonFile(const std::string& filename, const std::map<std::string, std::string>& data);

struct Config {
    std::string base_url;
    std::string api_key;
    std::string model;
    int workers;
    int timeout_seconds;
    int max_retries;
    int base_delay_ms;
    int max_delay_ms;
};

const Config DEFAULT_CONFIG = {
    "https://api.deepseek.com/v1/chat/completions",
    "",
    "deepseek-chat",
    20,
    60,
    3,
    1000,
    10000
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

class CurlGlobalInitializer {
public:
    CurlGlobalInitializer() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl");
        }
    }
    ~CurlGlobalInitializer() {
        curl_global_cleanup();
    }
};

class ConfigManager {
public:
    static Config loadConfig(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "警告: 无法打开配置文件 " << filename << "，使用默认配置。" << std::endl;
            return DEFAULT_CONFIG;
        }
        
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errors;
        
        if (!Json::parseFromStream(reader, file, &root, &errors)) {
            std::cerr << "警告: 配置文件解析错误: " << errors << "，使用默认配置。" << std::endl;
            return DEFAULT_CONFIG;
        }
        
        Config config;
        config.base_url = root.get("base_url", DEFAULT_CONFIG.base_url).asString();
        config.api_key = root.get("api_key", DEFAULT_CONFIG.api_key).asString();
        config.model = root.get("model", DEFAULT_CONFIG.model).asString();
        config.workers = root.get("workers", DEFAULT_CONFIG.workers).asInt();
        config.timeout_seconds = root.get("timeout_seconds", DEFAULT_CONFIG.timeout_seconds).asInt();
        config.max_retries = root.get("max_retries", DEFAULT_CONFIG.max_retries).asInt();
        config.base_delay_ms = root.get("base_delay_ms", DEFAULT_CONFIG.base_delay_ms).asInt();
        config.max_delay_ms = root.get("max_delay_ms", DEFAULT_CONFIG.max_delay_ms).asInt();
        
        if (config.api_key.empty() || config.api_key == "Your key") {
            std::cerr << "警告: API密钥未配置，请检查 " << filename << std::endl;
        }
        
        std::cout << "配置加载成功:" << std::endl;
        std::cout << "  - Base URL: " << config.base_url << std::endl;
        std::cout << "  - Model: " << config.model << std::endl;
        std::cout << "  - Workers: " << config.workers << std::endl;
        
        return config;
    }
    
    static void createDefaultConfig(const std::string& filename) {
        Json::Value root;
        root["base_url"] = DEFAULT_CONFIG.base_url;
        root["api_key"] = "Your key";
        root["model"] = DEFAULT_CONFIG.model;
        root["workers"] = DEFAULT_CONFIG.workers;
        root["timeout_seconds"] = DEFAULT_CONFIG.timeout_seconds;
        root["max_retries"] = DEFAULT_CONFIG.max_retries;
        root["base_delay_ms"] = DEFAULT_CONFIG.base_delay_ms;
        root["max_delay_ms"] = DEFAULT_CONFIG.max_delay_ms;
        
        std::ofstream file(filename);
        if (!file.is_open()) {
             throw std::runtime_error("无法创建配置文件: " + filename);
        }
        
        Json::StreamWriterBuilder writer;
        writer["emitUTF8"] = true; 
        writer["indentation"] = "  ";
        std::unique_ptr<Json::StreamWriter> jsonWriter(writer.newStreamWriter());
        jsonWriter->write(root, &file);
        
        std::cout << "已创建默认配置文件: " << filename << std::endl;
        std::cout << "请编辑该文件并设置你的API密钥。" << std::endl;
    }
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* response) {
    size_t totalSize = size * nmemb;
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

class HttpClient {
private:
    std::string base_url;
    std::string api_key;
    long timeout_seconds;
    
public:
    HttpClient(const Config& config) 
        : base_url(config.base_url), api_key(config.api_key), timeout_seconds(config.timeout_seconds) {}
    
    std::string post(const std::string& payload) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL handle");
        }
        
        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
        
        curl_easy_setopt(curl, CURLOPT_URL, base_url.c_str());
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

static std::atomic<bool> g_shutdown_requested(false);
static std::atomic<bool> g_force_exit(false);

void signal_handler(int signal) {
    static int signal_count = 0;
    signal_count++;
    
    if (signal_count == 1) {
        std::cout << "\n收到中断信号，正在优雅退出..." << std::endl;
        g_shutdown_requested = true;
    } else if (signal_count >= 2) {
        std::cout << "\n强制退出..." << std::endl;
        g_force_exit = true;
        std::quick_exit(1);
    }
}

class WorkerManager {
private:
    const std::vector<Job>& jobs;
    int concurrency;
    std::shared_ptr<HttpClient> client;
    std::string model;
    
    std::atomic<int> job_index;
    std::atomic<int> completed_count;
    std::mutex results_mutex;
    std::map<int, Result> results;
    
    struct Buffer {
        std::map<std::string, std::string> data;
        std::mutex mutex;
        bool active;
    };
    
    Buffer buffer1;
    Buffer buffer2;
    Buffer* write_buffer;
    Buffer* flush_buffer;
    std::thread flush_thread;
    std::atomic<bool> running;
    std::condition_variable flush_cv;
    std::mutex flush_mutex;
    const std::string& temp_output_file;
    std::mutex swap_mutex;
    
    int max_retries;
    int base_delay_ms;
    int max_delay_ms;
    std::mt19937 rng;
    
    void flushWorker();
    void saveToTempFile(const std::string& key, const std::string& text);
    void swapBuffers();
    void flushBufferData(Buffer* buffer);
    
    int calculateDelayWithJitter(int attempt) {
        int delay = base_delay_ms * (1 << (attempt - 1));
        delay = std::min(delay, max_delay_ms);
        std::uniform_int_distribution<int> dist(0, delay / 2);
        return delay + dist(rng);
    }
    
    bool checkShutdown() {
        if (g_force_exit) {
            std::quick_exit(1);
        }
        return g_shutdown_requested;
    }
    
public:
    WorkerManager(const std::vector<Job>& jobs_list, const Config& config, std::shared_ptr<HttpClient> http_client, const std::string& temp_file)
        : jobs(jobs_list), 
          concurrency(config.workers), 
          client(http_client), 
          model(config.model),
          job_index(0), 
          completed_count(0),
          temp_output_file(temp_file),
          max_retries(config.max_retries), 
          base_delay_ms(config.base_delay_ms), 
          max_delay_ms(config.max_delay_ms), 
          rng(std::random_device{}()),
          running(true) {
        
        buffer1.active = true;
        buffer2.active = false;
        write_buffer = &buffer1;
        flush_buffer = &buffer2;
        
        flush_thread = std::thread(&WorkerManager::flushWorker, this);
    }
    
    ~WorkerManager() {
        running = false;
        flush_cv.notify_all();
        
        if (flush_thread.joinable()) {
            auto start = std::chrono::steady_clock::now();
            if (flush_thread.joinable()) {
                flush_thread.join();
            }
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
            if (duration.count() > 5) {
                std::cerr << "警告: Flush线程等待时间过长" << std::endl;
            }
        }
        
        if (!write_buffer->data.empty()) {
            try {
                swapBuffers();
                flushBufferData(flush_buffer);
            } catch (...) {
                std::cerr << "警告: 最终数据刷新失败" << std::endl;
            }
        }
    }
    
    void requestShutdown() {
        g_shutdown_requested = true;
        running = false;
        flush_cv.notify_all();
    }
    
    std::string callAPI(const std::string& text) {
        if (checkShutdown()) {
            throw std::runtime_error("Shutdown requested");
        }
        
        Json::Value payload;
        payload["model"] = model;
        
        Json::Value messages(Json::arrayValue);
        Json::Value system_msg;
        system_msg["role"] = "system";
        system_msg["content"] = "你是中日翻译大师，现在你要将日文翻译成中文，现在你在翻译一个rpg游戏的内容，如果遇到非日文的文本，不用翻译，直接输出原文，请保持原格式，只是翻译文本";
        messages.append(system_msg);
        
        Json::Value user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = text;
        messages.append(user_msg);
        
        payload["messages"] = messages;
        
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        std::string payload_str = Json::writeString(writer, payload);
        
        for (int attempt = 1; attempt <= max_retries; ++attempt) {
            if (checkShutdown()) {
                throw std::runtime_error("Shutdown requested");
            }
            
            try {
                std::string response_str = client->post(payload_str);
                
                Json::Value response_json;
                Json::CharReaderBuilder reader;
                std::string errors;
                std::istringstream response_stream(response_str);
                
                if (!Json::parseFromStream(reader, response_stream, &response_json, &errors)) {
                    throw std::runtime_error("Failed to parse JSON response: " + errors);
                }
                
                if (response_json.isMember("error")) {
                    throw std::runtime_error("API Error: " + response_json["error"]["message"].asString());
                }

                if (response_json.isMember("choices") && response_json["choices"].isArray() && 
                    !response_json["choices"].empty() && response_json["choices"][0]["message"].isMember("content")) {
                    return response_json["choices"][0]["message"]["content"].asString();
                }
                
                throw std::runtime_error("Invalid or empty response format from API");
                
            } catch (const std::exception& e) {
                if (attempt == max_retries) {
                    throw std::runtime_error("Failed after " + std::to_string(max_retries) + " attempts. Last error: " + e.what());
                }
                
                int delay_ms = calculateDelayWithJitter(attempt);
                
                std::string text_preview = utf8_substr(text, 10); 
                
                std::cerr << "任务 \"" << text_preview << "...\" 尝试 " << attempt << " 失败: " << e.what() 
                << ", " << delay_ms << "ms 后重试" << std::endl;
                
                auto start = std::chrono::steady_clock::now();
                while (true) {
                    if (checkShutdown()) {
                        throw std::runtime_error("Shutdown requested");
                    }
                    
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    
                    if (elapsed >= delay_ms) {
                        break;
                    }
                    
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        throw std::runtime_error("Unexpected error in retry logic");
    }
    
    void worker() {
        try {
            while (!checkShutdown()) {
                int idx = job_index.fetch_add(1);
                if (idx >= jobs.size()) {
                    break;
                }
                
                const Job& job = jobs[idx];
                Result result;
                result.key = job.key;
                result.index = job.index;
                
                try {
                    result.text = callAPI(job.text);
                    saveToTempFile(job.key, result.text);
                    
                    int current_completed = completed_count.fetch_add(1) + 1;
                    float percentage = static_cast<float>(current_completed) / jobs.size() * 100.0f;
                    
                    std::stringstream ss;
                    ss << "\r进度: " << current_completed << "/" << jobs.size() 
                       << " [" << std::fixed << std::setprecision(2) << percentage << "%] - 完成: " << job.key;
                    std::cout << ss.str() << std::flush;
                    
                } catch (const std::exception& e) {
                    if (g_shutdown_requested && std::string(e.what()) == "Shutdown requested") {
                        result.error = "用户中断";
                    } else {
                        result.error = e.what();
                    }
                }
                
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results[result.index] = result;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Worker线程异常: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Worker线程未知异常" << std::endl;
        }
    }
    
    std::vector<Result> run() {
        std::vector<std::thread> workers;
        for (int i = 0; i < std::min(concurrency, (int)jobs.size()); ++i) {
            workers.emplace_back(&WorkerManager::worker, this);
        }
        
        for (auto& w : workers) {
            if (w.joinable()) {
                auto start = std::chrono::steady_clock::now();
                w.join();
                auto end = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
                if (duration.count() > 30) {
                    std::cerr << "警告: Worker线程等待时间过长" << std::endl;
                }
            }
        }
        std::cout << std::endl;
        
        std::vector<Result> sorted_results;
        {
            std::lock_guard<std::mutex> lock(results_mutex);
            for (const auto& pair : results) {
                sorted_results.push_back(pair.second);
            }
        }
        return sorted_results;
    }
};

void WorkerManager::saveToTempFile(const std::string& key, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(write_buffer->mutex);
        write_buffer->data[key] = text;
    }
    
    if (write_buffer->data.size() >= 10) {
        swapBuffers();
    }
}

void WorkerManager::swapBuffers() {
    std::unique_lock<std::mutex> swap_lock(swap_mutex);
    
    {
        std::lock_guard<std::mutex> lock1(write_buffer->mutex);
        std::lock_guard<std::mutex> lock2(flush_buffer->mutex);
        
        Buffer* temp = write_buffer;
        write_buffer = flush_buffer;
        flush_buffer = temp;
    }
    
    flush_cv.notify_one();
}

void WorkerManager::flushWorker() {
    try {
        while (running) {
            std::unique_lock<std::mutex> lock(flush_mutex);
            if (!flush_cv.wait_for(lock, std::chrono::seconds(5), [this] { 
                return !flush_buffer->data.empty() || !running; 
            })) {
                continue;
            }
            
            if (!running && flush_buffer->data.empty()) {
                break;
            }
            
            if (!flush_buffer->data.empty()) {
                flushBufferData(flush_buffer);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Flush线程异常: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Flush线程未知异常" << std::endl;
    }
}

void WorkerManager::flushBufferData(Buffer* buffer) {
    std::map<std::string, std::string> to_write;
    {
        std::lock_guard<std::mutex> lock(buffer->mutex);
        if (buffer->data.empty()) return;
        to_write = std::move(buffer->data);
        buffer->data.clear();
    }
    
    std::string temp_file_path = temp_output_file + ".tmp";
    std::ofstream temp_file(temp_file_path);
    
    if (!temp_file.is_open()) {
        std::cerr << "警告: 无法创建临时文件 " << temp_file_path << std::endl;
        return;
    }
    
    std::map<std::string, std::string> existing_data;
    try {
        std::ifstream current_file(temp_output_file);
        if (current_file.is_open()) {
            current_file.close();
            existing_data = readJsonFile(temp_output_file);
        }
    } catch (...) {
    }
    
    for (const auto& pair : to_write) {
        existing_data[pair.first] = pair.second;
    }
    
    Json::Value root;
    for (const auto& pair : existing_data) {
        root[pair.first] = pair.second;
    }
    
    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> jsonWriter(writer.newStreamWriter());
    jsonWriter->write(root, &temp_file);
    temp_file.close();
    
    if (std::rename(temp_file_path.c_str(), temp_output_file.c_str()) != 0) {
        std::cerr << "警告: 无法更新临时文件 " << temp_output_file << std::endl;
        std::remove(temp_file_path.c_str());
    }
}

std::map<std::string, std::string> readJsonFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开文件: " + filename);
    }
    
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    if (!Json::parseFromStream(reader, file, &root, &errors)) {
        throw std::runtime_error("解析JSON失败: " + errors);
    }
    
    std::map<std::string, std::string> result;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it->isString()) {
            result[it.key().asString()] = it->asString();
        }
    }
    return result;
}

void writeJsonFile(const std::string& filename, const std::map<std::string, std::string>& data) {
    Json::Value root;
    for (const auto& pair : data) {
        root[pair.first] = pair.second;
    }

    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("无法写入文件: " + filename);
    }

    Json::StreamWriterBuilder writer;
    writer["emitUTF8"] = true;
    writer["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> jsonWriter(writer.newStreamWriter());
    jsonWriter->write(root, &file);
}

void print_usage(const char* prog_name) {
    std::cerr << "用法: " << prog_name << " [选项]\n\n"
              << "选项:\n"
              << "  -i, --input <file>      输入JSON文件 (默认: trans.json)\n"
              << "  -o, --output <file>     输出JSON文件 (默认: trans_output.json)\n"
              << "  -c, --config <file>     配置文件 (默认: config.json)\n"
              << "  --create-config         创建默认的 config.json 文件并退出\n"
              << "  -h, --help              显示此帮助信息\n";
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    std::string input_file = "trans.json";
    std::string output_file = "trans_output.json";
    std::string config_file = "config.json";
    const std::string temp_output_file = "trans_output_temp.json";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--create-config") {
            try {
                ConfigManager::createDefaultConfig(config_file);
            } catch (const std::exception& e) {
                std::cerr << "错误: " << e.what() << std::endl;
                return 1;
            }
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            input_file = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_file = argv[++i];
        } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_file = argv[++i];
        }
    }

    CurlGlobalInitializer curl_initializer;
    
    try {
        Config config = ConfigManager::loadConfig(config_file);
        if (config.api_key.empty() || config.api_key == "Your key") {
             throw std::runtime_error("API Key 未在 " + config_file + " 中配置。");
        }
        
        auto original_texts = readJsonFile(input_file);
        
        std::map<std::string, std::string> completed_translations;
        std::ifstream temp_file_check(temp_output_file);
        if (temp_file_check.is_open()) {
            temp_file_check.close();
            try {
                completed_translations = readJsonFile(temp_output_file);
                if (!completed_translations.empty()) {
                    std::cout << "从临时文件恢复了 " << completed_translations.size() << " 条已完成的翻译。" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "警告: 读取临时文件失败，将重新开始翻译: " << e.what() << std::endl;
            }
        }
        
        std::vector<Job> jobs;
        int index = 0;
        for (const auto& pair : original_texts) {
            if (completed_translations.find(pair.first) == completed_translations.end()) {
                jobs.push_back({pair.first, pair.second, index++});
            }
        }
        
        if (jobs.empty()) {
            std::cout << "所有翻译任务均已完成。" << std::endl;
            if (!completed_translations.empty()) {
                writeJsonFile(output_file, completed_translations);
                std::cout << "结果已写入 " << output_file << std::endl;
                std::remove(temp_output_file.c_str());
            }
            return 0;
        }
        
        std::cout << "总任务: " << original_texts.size() << "，需要翻译: " << jobs.size() << "，已跳过: " << completed_translations.size() << std::endl;
        
        auto client = std::make_shared<HttpClient>(config);
        std::unique_ptr<WorkerManager> manager;
        
        try {
            manager = std::make_unique<WorkerManager>(jobs, config, client, temp_output_file);
        } catch (...) {
            throw;
        }
        
        auto results = manager->run();
        
        if (g_shutdown_requested) {
            std::cout << "\n检测到中断信号，正在保存进度..." << std::endl;
        }
        
        std::map<std::string, std::string> final_output = completed_translations;
        int fail_count = 0;
        
        for (const auto& result : results) {
            if (result.error.empty()) {
                final_output[result.key] = result.text;
            } else {
                std::cerr << "\n错误: key='" << result.key << "' 翻译失败: " << result.error << std::endl;
                fail_count++;
            }
        }
        
        writeJsonFile(output_file, final_output);
        std::remove(temp_output_file.c_str());
        
        if (g_shutdown_requested) {
            std::cout << "\n程序已中断，已保存当前进度到 " << output_file << std::endl;
            std::cout << "成功: " << final_output.size() << ", 失败: " << fail_count << std::endl;
            return 1;
        }
        
        std::cout << "\n全部翻译完成，结果已写入 " << output_file << std::endl;
        std::cout << "成功: " << final_output.size() << ", 失败: " << fail_count << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "发生严重错误: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "发生未知严重错误" << std::endl;
        return 1;
    }
    
    return 0;
}
