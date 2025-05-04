#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Базовый класс для всех метрик
class Metric {
public:
    virtual ~Metric() {}
    virtual std::vector<std::string> collect() = 0;
};

// Базовый класс для всех выводов
class Output {
public:
    virtual ~Output() {}
    virtual void write(const std::string& data) = 0;
};

// Класс для работы с конфиг файлом
class Config {
private:
    int period;
    std::vector<json> metrics_config;
    std::vector<json> outputs_config;

public:
    int get_period() { return period; }
    auto& get_metrics_config() { return metrics_config; }
    auto& get_outputs_config() { return outputs_config; }

    static Config read_config(const std::string& filename) {
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("Ошибка открытия файла: " + filename);
        
        json config_json = json::parse(file);
        Config config;

        if (!config_json.contains("settings") || 
            !config_json["settings"].contains("period") ||
            !config_json.contains("metrics") || 
            !config_json.contains("outputs")) {
            throw std::runtime_error("Неверная структура конфигурации");
        }

        config.period = std::stoi(config_json["settings"]["period"].get<std::string>());
        if (config.period <= 0) throw std::runtime_error("Некорректный период");

        config.metrics_config = config_json["metrics"].get<std::vector<json>>();
        config.outputs_config = config_json["outputs"].get<std::vector<json>>();

        return config;
    }
};

// Класс для сбора данных о CPU
class CpuMetric : public Metric {
private:
    std::vector<int> cpu_ids;
    std::vector<long> prev_total;
    std::vector<long> prev_active;

public:
    CpuMetric(const std::vector<int>& ids) : cpu_ids(ids) {
        prev_total.resize(ids.size(), 0);
        prev_active.resize(ids.size(), 0);
    }

    std::vector<std::string> collect() override {
        std::vector<std::string> result;
        std::vector<bool> cpu_found(cpu_ids.size(), false);
        std::ifstream file("/proc/stat");
        
        if (!file) throw std::runtime_error("Ошибка открытия файла /proc/stat");

        std::string line;
        while (std::getline(file, line)) {
            if (line.find("cpu") != 0 || line.find("cpu ") == 0) continue;
            int cpu_num = std::stoi(line.substr(3, line.find(' ') - 3));

            for (int i = 0; i < cpu_ids.size(); ++i) {
                if (cpu_ids[i] == cpu_num) {
                    std::istringstream iss(line);
                    long user, nice, system, idle, iowait, irq, softirq, steal;
                    std::string cpu;
                    try{
                        iss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
                    } catch (const std::exception& e) {
                        throw std::runtime_error("Ошибка парсинга строки: " + line);
                    }

                    // Рассчитываем общее и активное время
                    long total = user + nice + system + idle + iowait + irq + softirq + steal;
                    long active = user + nice + system + irq + softirq + steal;

                    /* 
                    Расчёт загрузки CPU за период между измерениями 
                    как отношение времени использования процессора к общему времени процессора
                    Где:
                        active - prev_active[i] 
                        разница активного времени между замерами
                        total - prev_total[i]    
                        разница общего времени между замерами
                    */
                    if (prev_total[i] > 0){
                        if (total != prev_total[i]) {
                            double usage = 100.0 * (active - prev_active[i]) / (total - prev_total[i]);
                            std::ostringstream oss;
                            oss << std::fixed << std::setprecision(2) << usage;
                            result.push_back("Cpu" + std::to_string(cpu_ids[i]) + ": " + oss.str() + " %\n");
                        }
                        else {
                            result.push_back("Интервал между измерениями для Cpu" + std::to_string(cpu_ids[i]) + " не достаточен\n");
                        } 
                    }
                    // Первое измерение
                    else{
                        double usage = 100.0 * active / total;
                        std::ostringstream oss;
                        oss << std::fixed << std::setprecision(2) << usage;
                        result.push_back("Cpu" + std::to_string(cpu_ids[i]) + ": " + oss.str() + " %\n");
                    }

                    // Сохраняем текущие значения для следующего измерения
                    prev_total[i] = total;
                    prev_active[i] = active;
                    cpu_found[i] = true;
                    break;
                }
            }
        }

        // Проверка отсутствующих ядер
        for(int i = 0; i < cpu_ids.size(); ++i) {
            if(cpu_found[i] == false) result.push_back("Ошибка: Cpu" + std::to_string(cpu_ids[i]) + " не найден!\n");
        }

        return result;
    }
};

// Класс для сбора данных о памяти
class MemoryMetric : public Metric {
private:
    bool check_used;
    bool check_free;

public:
    MemoryMetric(const std::vector<std::string>& specs) {
        check_used = std::find(specs.begin(), specs.end(), "used") != specs.end();
        check_free = std::find(specs.begin(), specs.end(), "free") != specs.end();
    }

    std::vector<std::string> collect() override {
        std::vector<std::string> result;
        std::ifstream file("/proc/meminfo");
        
        if (!file) throw std::runtime_error("Ошибка открытия файла /proc/meminfo");

        long total = 0, free = 0, buffers = 0, cached = 0;
        std::string line;

        while (std::getline(file, line)) {
            if (line.find("MemTotal") == 0) total = extract_value(line);
            if (line.find("MemFree") == 0) free = extract_value(line);  
            if (line.find("Buffers") == 0) buffers = extract_value(line);
            if (line.find("Cached") == 0) cached = extract_value(line);
        }

        /*  
        Рассчитываем используемую память
        Total - общий размер оперативной памяти (ОЗУ) в системе
        Free - свободная память
        Buffers - буферы в памяти 
        Cached - кеш файлов
        */
        long used = total - free - buffers - cached;

        if (check_used) {
            result.push_back("Используемая память: " + std::to_string(used) + " kB\n");
        }
        if (check_free) {
            result.push_back("Свободная память: " + std::to_string(free) + " kB\n");
        }

        return result;
    }

    long extract_value(const std::string& line) {
        std::istringstream iss(line);
        std::string key;
        long value;
        try{
            iss >> key >> value;  
        } catch (const std::exception& e) {
            throw std::runtime_error("Ошибка парсинга строки: " + line);
        }
        return value;
    }
};

// Вывод результатов в консоль
class ConsoleOutput : public Output {
public:
    void write(const std::string& data) override {
        std::cout << data;
        std::cout.flush();
    }
};

// Вывод результатов в файл
class LogOutput : public Output {
private:
    std::ofstream log_file;
public:
    LogOutput(const std::string& path) {
        log_file.open(path, std::ios::app);
        if (!log_file) throw std::runtime_error("Ошибка открытия лог файла: " + path);
    }

    void write(const std::string& data) override {
        log_file << data;
        log_file.flush();
    }

    ~LogOutput() {
        if (log_file.is_open()) {
            log_file.close();
        }
    }
};

// Фабрика для создания метрик
class MetricFactory {
public:
    static Metric* create(const json& config){
        std::string type = config["type"];
        if (type == "cpu") {
            return new CpuMetric(config["ids"].get<std::vector<int>>());
        }
        if (type == "memory") {
            return new MemoryMetric(config["spec"].get<std::vector<std::string>>());
        }
        throw std::runtime_error("Неизвестный тип метрики: " + type);
    }  
};

// Фабрика для создания выводов
class OutputFactory {
public:
    static Output* create(const json& config){
        std::string type = config["type"];
        if (type == "console") {
            return new ConsoleOutput();
        }
        if (type == "log") {
            return new LogOutput(config["path"].get<std::string>());
        }
        throw std::runtime_error("Неизвестный тип вывода: " + type);
    }
};

// Вывод локального времени
std::string get_time() {
    time_t now = time(nullptr);
    std::string time_str = ctime(&now);
    return time_str;
}

int main() {
    std::vector<Metric*> metrics;
    std::vector<Output*> outputs;

    try {
        Config config = Config::read_config("config.json");

        // Создание метрик через фабрику
        for (const auto& metric_config : config.get_metrics_config()) {
            metrics.push_back(MetricFactory::create(metric_config));
        }

        // Создание выводов через фабрику
        for (const auto& output_config : config.get_outputs_config()) {
            outputs.push_back(OutputFactory::create(output_config));
        }

        while (true) {
            std::string result = get_time();

            // Сбор данных
            for (auto* metric : metrics) {
                for (const auto& line : metric->collect()) {
                    result += line;
                }
            }

            // Вывод данных
            for (auto* output : outputs) {
                output->write(result);
            }

            sleep(config.get_period());
        }
    }
    catch (const std::exception& e) {
        // Очистка памяти при ошибке
        for (const auto* metric : metrics) delete metric;
        for (const auto* output : outputs) delete output;
        std::cerr << "Ошибка: " << e.what() << std::endl;
        return 1;
    }

    // Очистка памяти при нормальном завершении
    for (const auto* metric : metrics) delete metric;
    for (const auto* output : outputs) delete output;
    return 0;
}