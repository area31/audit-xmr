// audit-xmr.cpp
#include "audit.hpp"
#include "rpc.hpp"
#include "log.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <optional>
#include <cstdlib>
#if __cplusplus >= 201703L
    #include <filesystem>
    namespace fs = std::filesystem;
#else
    #include <experimental/filesystem>
    namespace fs = std::experimental::filesystem;
#endif
#include <map>
#include <chrono>
#include <iomanip>
#include <condition_variable>

#define VER "0.1"

std::string g_log_path;
std::mutex cout_mutex; // Para sincronizar saída no terminal
std::atomic<int> blocks_written(0); // Contador global de blocos escritos

std::map<std::string, std::string> load_config(const std::string& config_file) {
    std::map<std::string, std::string> config;
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "[AVISO] Não foi possível abrir o arquivo de configuração " << config_file << ". Usando padrões.\n";
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string key, value;
        if (std::getline(iss, key, '=') && std::getline(iss, value)) {
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            config[key] = value;
        }
    }
    file.close();
    return config;
}

// Função para exibir a barra de progresso
void print_progress(int current, int total) {
    const int bar_width = 20;
    float progress = (float)current / total;
    int pos = bar_width * progress;

    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\rProgresso: [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "=";
        else if (i == pos) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << int(progress * 100.0) << "% (" << current << "/" << total << ")" << std::flush;
}

int main(int argc, char* argv[]) {
    std::string config_file = "audit-xmr.cfg";
    auto config = load_config(config_file);

    std::string rpc_url;
    if (config.count("server")) {
        std::string server_cfg = config["server"];
        if (server_cfg.find(":") != std::string::npos) {
            rpc_url = "http://" + server_cfg + "/json_rpc";
        } else {
            rpc_url = "http://" + server_cfg + ":18081/json_rpc";
        }
    } else if (config.count("rpc_url")) {
        rpc_url = config["rpc_url"];
    } else {
        rpc_url = "http://127.0.0.1:18081/json_rpc";
    }

    int user_thread_count = config.count("threads") ? std::stoi(config["threads"]) : 1;
    std::string output_dir = config.count("output_dir") ? config["output_dir"] : "out";

    int start_block = -1;
    int end_block = -1;
    int single_block = -1;
    bool args_specified = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--range" && i + 2 < argc) {
            start_block = std::stoi(argv[++i]);
            end_block = std::stoi(argv[++i]);
            args_specified = true;
        } else if (arg == "--block" && i + 1 < argc) {
            single_block = std::stoi(argv[++i]);
            args_specified = true;
        } else if (arg == "--threads" && i + 1 < argc) {
            std::string tval = argv[i + 1];
            if (tval == "max") {
                user_thread_count = std::thread::hardware_concurrency();
                ++i;
            } else {
                user_thread_count = std::stoi(tval);
                ++i;
            }
        } else if (arg == "--server" && i + 1 < argc) {
            std::string server = argv[++i];
            if (server.find(":") != std::string::npos) {
                rpc_url = "http://" + server + "/json_rpc";
            } else {
                rpc_url = "http://" + server + ":18081/json_rpc";
            }
        } else if (arg == "--output-dir" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "\nUso: ./audit-xmr [opções]\n"
                      << "  --range <inicio> <fim>     Audita blocos do início ao fim\n"
                      << "  --block <altura>           Audita apenas um bloco específico\n"
                      << "  --threads <N>|max          Define o número de threads\n"
                      << "  --server <ip[:porta]>      Define o servidor RPC\n"
                      << "  --output-dir <dir>         Define o diretório de saída\n"
                      << "  -h, --help                 Mostra esta ajuda\n"
                      << "  -v, --version              Mostra a versão\n";
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "Versão: " << VER << std::endl;
            return 0;
        }
    }

    set_rpc_url(rpc_url);

    fs::path out_dir = fs::path(output_dir);
    fs::create_directories(out_dir);

    std::string csv_path = (out_dir / "auditoria_monero.csv").string();
    std::string log_path = (out_dir / "audit_log.txt").string();
    g_log_path = log_path;

    auto log = [&](const std::string& msg, bool is_block_end = false) {
        log_message(log_path, msg, is_block_end);
    };

    // Exibir configurações
    std::cout << "------------------------\n";
    std::cout << "Configurações do audit-xmr\n";
    std::cout << "------------------------\n";
    std::cout << "RPC URL: " << rpc_url << "\n";
    std::cout << "  (Origem: " << (config.count("rpc_url") ? "audit-xmr.cfg" : config.count("server") ? "audit-xmr.cfg (server)" : "--server ou padrão") << ")\n";
    std::cout << "Threads: " << user_thread_count << "\n";
    std::cout << "  (Origem: " << (config.count("threads") ? "audit-xmr.cfg" : "--threads ou padrão") << ")\n";
    std::cout << "Output Dir: " << output_dir << "\n";
    std::cout << "  (Origem: " << (config.count("output_dir") ? "audit-xmr.cfg" : "--output-dir ou padrão") << ")\n";
    if (config.count("max_retries")) std::cout << "Max Retries: " << config["max_retries"] << " (audit-xmr.cfg)\n";
    if (config.count("timeout")) std::cout << "Timeout: " << config["timeout"] << " (audit-xmr.cfg)\n";
    std::cout << "CSV Path: " << csv_path << "\n";
    std::cout << "Log Path: " << log_path << "\n";
    std::cout << "------------------------\n\n";

    log("[INFO] Script iniciado");

    // Variáveis para controle de escrita ordenada
    std::mutex csv_mutex;
    std::condition_variable csv_cv;
    std::map<int, AuditResult> pending_results;
    int next_block_to_write = 0;

    // Inicializa o CSV com o cabeçalho
    {
        std::lock_guard<std::mutex> lock(csv_mutex);
        std::ofstream csv(csv_path);
        if (!csv.is_open()) {
            std::cerr << "[ERRO] Não foi possível abrir o arquivo CSV para escrita: " << csv_path << std::endl;
            log("[ERRO] Não foi possível abrir o arquivo CSV para escrita: " + csv_path);
            return 1;
        }
        csv << "Altura,Hash,RecompensaReal,CoinbaseOutputs,TotalMinerado,Problemas,Status\n";
        csv.close();
    }

    auto write_to_csv = [&](const AuditResult& res, int total_blocks) {
        std::unique_lock<std::mutex> lock(csv_mutex);
        pending_results[res.height] = res;

        while (pending_results.find(next_block_to_write) != pending_results.end()) {
            const auto& r = pending_results[next_block_to_write];
            std::ofstream csv(csv_path, std::ios::app);
            if (!csv.is_open()) {
                std::cerr << "[ERRO] Não foi possível abrir o arquivo CSV para escrita: " << csv_path << std::endl;
                log("[ERRO] Não foi possível abrir o arquivo CSV para escrita: " + csv_path);
                return;
            }
            csv << r.height << ',' << r.hash << ',' << r.real_reward << ','
                << r.coinbase_outputs << ',' << r.total_mined << ','
                << (r.issues.empty() ? "Nenhum" : r.issues_string()) << ',' << r.status << '\n';
            csv.close();

            log("[INFO] Bloco " + std::to_string(r.height) + " escrito no CSV: status=" + r.status);
            blocks_written++; // Incrementa o contador global
            if (total_blocks > 0) { // Só exibe progresso no modo intervalo
                print_progress(blocks_written, total_blocks);
            }

            pending_results.erase(next_block_to_write);
            next_block_to_write++;
        }
        lock.unlock();
        csv_cv.notify_all();
    };

    if (single_block >= 0) {
        std::cout << "------------------------\n";
        std::cout << "Auditoria de Bloco Único\n";
        std::cout << "------------------------\n";
        log("[INFO] Auditando bloco único: " + std::to_string(single_block));
        auto res = audit_block(single_block);
        if (res.has_value()) {
            auto result = res.value();
            std::cout << "Bloco " << result.height << ":\n";
            std::cout << "  Hash: " << result.hash << "\n";
            std::cout << "  Recompensa Real: " << result.real_reward << "\n";
            std::cout << "  Saídas Coinbase: " << result.coinbase_outputs << "\n";
            std::cout << "  Total Minerado: " << result.total_mined << "\n";
            std::cout << "  Problemas: " << (result.issues.empty() ? "Nenhum" : result.issues_string()) << "\n";
            std::cout << "  Status: " << result.status << "\n";

            std::ofstream csv(csv_path, std::ios::app);
            if (!csv.is_open()) {
                std::cerr << "[ERRO] Não foi possível abrir o arquivo CSV para escrita: " << csv_path << std::endl;
                log("[ERRO] Não foi possível abrir o arquivo CSV para escrita: " + csv_path);
                return 1;
            }
            csv << result.height << ',' << result.hash << ',' << result.real_reward << ','
                << result.coinbase_outputs << ',' << result.total_mined << ','
                << (result.issues.empty() ? "Nenhum" : result.issues_string()) << ',' << result.status << '\n';
            csv.close();
            log("[INFO] Bloco " + std::to_string(result.height) + " escrito no CSV: status=" + result.status);
            std::cout << "Bloco " << std::setw(6) << result.height << " escrito no CSV\n";
        } else {
            std::cerr << "[ERRO] Auditoria falhou para o bloco " << single_block << std::endl;
            log("[ERRO] Auditoria falhou para o bloco " + std::to_string(single_block), true);
            return 1;
        }
    } else {
        if (!args_specified) {
            start_block = 0;
            end_block = get_blockchain_height() - 1;
            if (end_block < 0) {
                std::cerr << "[ERRO] Não foi possível obter a altura da blockchain." << std::endl;
                log("[ERRO] Falha ao obter altura da blockchain via RPC.");
                return 1;
            }
            log("[INFO] Nenhum argumento fornecido. Auditando todos os blocos de 0 a " + std::to_string(end_block));
        }

        std::cout << "------------------------\n";
        std::cout << "Auditoria de Intervalo\n";
        std::cout << "------------------------\n";
        std::cout << "Auditando blocos de " << start_block << " a " << end_block << "\n";
        log("[INFO] Iniciando auditoria de " + std::to_string(start_block) + " até " + std::to_string(end_block));

        int max_threads = std::thread::hardware_concurrency();
        if (user_thread_count > max_threads) {
            std::cerr << "[AVISO] Threads solicitadas excedem o máximo do sistema (" << max_threads << "). Usando " << user_thread_count << ".\n";
        }

        int thread_count = std::max(1, user_thread_count);
        int total_blocks = end_block - start_block + 1;
        next_block_to_write = start_block;
        blocks_written = 0; // Inicializa o contador

        auto worker = [&](int tid, int from, int to) {
            for (int h = from; h <= to; ++h) {
                log("[DEBUG] Thread " + std::to_string(tid) + " auditando bloco " + std::to_string(h));
                auto res = audit_block(h);
                if (res.has_value()) {
                    write_to_csv(res.value(), total_blocks);
                } else {
                    log("[ERRO] Falha na auditoria do bloco " + std::to_string(h), true);
                }
            }
        };

        std::vector<std::thread> threads;
        int chunk_size = total_blocks / thread_count;
        int remainder = total_blocks % thread_count;
        int current = start_block;
        for (int i = 0; i < thread_count; ++i) {
            int extra = (i < remainder) ? 1 : 0;
            int range_size = chunk_size + extra;
            int from = current;
            int to = current + range_size - 1;
            current = to + 1;
            threads.emplace_back(worker, i, from, to);
        }

        for (auto& t : threads) t.join();
        std::cout << "\n"; // Nova linha após o progresso
    }

    std::cout << "------------------------\n";
    std::cout << "Auditoria Concluída\n";
    std::cout << "------------------------\n";
    std::cout << "Resultados salvos em: " << csv_path << "\n";
    std::cout << "------------------------\n";
    log("[INFO] Auditoria finalizada. Resultados salvos em: " + csv_path);

    return 0;
}
