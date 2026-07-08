#ifdef _WIN32
#ifndef  _CRT_SECURE_NO_WARNINGS
#define  _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib") 
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <iostream>
#include <vector>
#include <utility>
#include <mutex>
#include <string>
#include <filesystem>
#include <cstdint>

#include <thread>
#include <algorithm>
#include <queue>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <map>
#include <type_traits>

#include <chrono>
#include <sstream>
#include <fstream>
#include <iomanip>

#include <opencv2/opencv.hpp>

namespace fs = std::filesystem;

// 스레드 풀 클래스
class ThreadPool
{
public:
    // 생성자에서 스레드 수량과 최대 큐 크기를 인자로 받음 - 기본값 100
    ThreadPool(size_t threads, size_t max_queue = 100) : maxQueueSize(max_queue)
    {
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back([this]
                {
                    for (;;)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);

                            // 워커 스레드는 큐에 작업이 들어오거나 종료 신호가 올 때까지 대기
                            this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });

                            if (this->stop && this->tasks.empty()) return;

                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }

                        // 큐에서 작업을 가져오면 대기하던 스레드가 있는 경우 깨움
                        this->producer_condition.notify_one();

                        task(); // 실제 작업 수행
                    }
                });
    }

    template<class F, class... Args>
    auto enqueue(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) throw std::runtime_error("enqueue on stopped thread pool");
            
            // 큐에 들어있는 작업 개수가 maxQueueSize보다 작아질 때까지 스레드 대기
            producer_condition.wait(lock, [this] { return this->stop || this->tasks.size() < this->maxQueueSize; });
            if (stop) throw std::runtime_error("enqueue on stopped thread pool");
            
            tasks.emplace([task]() { (*task)(); });
        }

        // 워커 스레드에게 큐에 새 작업이 들어왔다고 알려줌
        condition.notify_one();

        return res;
    }

    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }

        condition.notify_all();
        producer_condition.notify_all();

        for (std::thread &worker : workers)
        {
            if (worker.joinable()) worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;          // 워커 스레드를 깨우기 위한 조건 변수

    std::condition_variable producer_condition; // 스레드를 깨우기 위한 조건 변수
    size_t maxQueueSize;                        // 큐의 최대 크기

    bool stop = false;
};

// 결과 순서 조정 클래스
class ReorderBuf
{
private:
    std::map<int, std::vector<unsigned char>> buffer;
    int expected_index = 0;
    std::mutex mtx;
    std::condition_variable cv;

public:
    // 결과 무작위 저장
    // 워커 스레드가 작업 완료 후 호출 - 순서 상관없음
    void push(int index, std::vector<unsigned char> data) 
    {
        std::lock_guard<std::mutex> lock(mtx);
        buffer[index] = std::move(data);
        
        // 스레드 깨우기
        cv.notify_one(); 
    }

    // 순서대로 데이터 꺼냄
    std::vector<unsigned char> pop_next() 
    {
        std::unique_lock<std::mutex> lock(mtx);
        
        // expected_index 번호가 맵에 존재할 때까지 대기
        cv.wait(lock, [this]() { return buffer.count(expected_index) > 0; });

        // 원하는 번호가 존재하면 데이터 추출
        auto data = std::move(buffer[expected_index]);
        buffer.erase(expected_index);
        
        // 기다릴 번호 갱신
        expected_index++; 
        
        return data;
    }
};

// 메모리 사용량 측정 클래스
class MemMonitor
{
private:
    std::atomic<bool> running{ false };
    std::thread monitorThread;
    std::vector<size_t> memoryUsage;

    // 현재 시간 문자열 생성
    std::string GetCurrentTimeString() const
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::localtime(&now_time);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d-%H%M%S");
        return oss.str();
    }

    // 메모리 사용량 측정
    size_t GetCurrentRSSKB() const
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            return pmc.WorkingSetSize / 1024; // byte를 kb로 변환
        }
        return 0;
#elif defined(__linux__)
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line))
        {
            if (line.rfind("VmRSS:", 0) == 0)
            {
                std::istringstream iss(line);
                std::string key, value, unit;
                size_t rssKB;
                iss >> key >> rssKB >> unit;
                return rssKB;
            }
        }
        return 0;
#else
        // 지원하지 않는 OS
        return 0;
#endif
    }

    // 스레드 내부에서 반복 측정
    void MonitorLoop(int intervalMs, bool realTimeSave, std::string filename)
    {
        std::ofstream out;
        if (realTimeSave)
        {
            out.open(filename);
            out << "index,memoryKB\n";
        }

        int index = 0;
        while (running.load())
        {
            size_t rss = GetCurrentRSSKB();

            if (realTimeSave)
            {
                out << ++index << "," << rss << "\n";
                out.flush();
            }
            else
            {
                memoryUsage.push_back(rss);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
        }

        if (realTimeSave && out.is_open())
        {
            out.close();
        }
    }

public:
    MemMonitor() = default;

    ~MemMonitor()
    {
        stop();
    }

    // 메모리 사용량 측정 시작
    // 기본 간격 500ms
    // 파일에 즉시 기록 혹은 메모리에 모았다가 종료 시 저장
    void start(int intervalMs = 500, bool realTimeSave = false, std::string fileName = "")
    {
        if (running.load())
        {
            return;
        }

        if (fileName.empty())
        {
            fileName = GetCurrentTimeString() + "-mem.csv";
        }

        memoryUsage.clear();
        running = true;

        // 스레드 생성, 실행
        monitorThread = std::thread(&MemMonitor::MonitorLoop, this, intervalMs, realTimeSave, fileName);
        std::cout << "memory usage exam start" << std::endl;
    }

    // 종료와 저장
    void stop(std::string fileName = "")
    {
        if (!running.load()) return;

        running = false;

        if (monitorThread.joinable())
        {
            monitorThread.join();
        }

        // 실시간 저장이 아니면 종료 후 데이터를 한번에 파일로 저장
        if (!memoryUsage.empty())
        {
            if (fileName.empty()) fileName = GetCurrentTimeString() + "-mem.csv";

            std::ofstream out(fileName);
            out << "memory kb\n";
            for (size_t i = 0; i < memoryUsage.size(); ++i)
            {
                out << (i + 1) << "," << memoryUsage[i] << "\n";
            }
            out.close();
            std::cout << "memory usage data saved : " << fileName << "\n";
            memoryUsage.clear();
        }
        else
        {
            std::cout << "memory usage exam done\n";
        }
    }
};

// 파일 저장
int WriteFileFromBuf(const std::string& filename, const void* data, size_t size)
{
    if (!data || size == 0) return -1;

    FILE* fp = fopen(filename.c_str(), "wb");
    if (!fp) {
        std::cerr << "file open for writing failed\n";
        return -2;
    }

    size_t written = fwrite(data, 1, size, fp);

    if (written != size) {
        std::cerr << "file write failed\n";
        fclose(fp);
        return -3;
    }

    fclose(fp);
    return 0;
}

// 파일 읽기
int ReadFileToVecBuf(const std::string& filename, std::vector<unsigned char>& vBuf)
{
    FILE* fp = fopen(filename.c_str(), "rb");
    if (!fp) {
        std::cerr << "file opening failed\n";
        return -1;
    }

#ifdef _WIN32
    _fseeki64(fp, 0, SEEK_END);
    long long fileSize = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
#else
    fseeko(fp, 0, SEEK_END);
    off_t fileSize = ftello(fp);
    fseeko(fp, 0, SEEK_SET);
#endif

    if (fileSize < 0)
    {
        std::cerr << "file size calculation failed\n";
        fclose(fp);
        return -2;
    }

    if (static_cast<unsigned long long>(fileSize) > SIZE_MAX)
    {
        std::cerr << "file is too large to memory\n";
        fclose(fp);
        return -3;
    }

    size_t targetSize = static_cast<size_t>(fileSize);
    if (vBuf.size() != targetSize) {
        vBuf.resize(targetSize);
    }

    size_t readSize = fread(vBuf.data(), 1, targetSize, fp);
    if (readSize != targetSize)
    {
        std::cerr << "file read size fail\n";
        fclose(fp);
        vBuf.clear();
        return -4;
    }

    fclose(fp);
    return 0;
}

// 확장자 기준으로 폴더의 파일 목록 작성
std::vector<std::string> ListFileWithExt(const std::string& folderPath, const std::string& extension)
{
    std::vector<std::string> files;

    if (!fs::exists(folderPath) || !fs::is_directory(folderPath))
    {
        std::cerr << "wrong path\n";
        return files;
    }

    for (const auto& entry : fs::directory_iterator(folderPath))
    {
        if (fs::is_regular_file(entry.path()))
        {
            if (entry.path().extension() == extension)
            {
                files.push_back(entry.path().generic_string());
            }
        }
    }

    std::sort(files.begin(), files.end());
    for (const auto& file : files)
    {
        std::cout << file << std::endl;
    }

    return files;
}

// jpg 파일을 로드하고 png 파일로 저장
void JpgToPng(const std::string& imgPathFile, const std::string& savePath)
{
    int rt = 0;
    std::vector<unsigned char> buf;
    
    rt = ReadFileToVecBuf(imgPathFile, buf);
    if (rt != 0)
    {
        std::cout << "image file load fail=" << imgPathFile << std::endl;
        return;
    }

    cv::Mat cvImg = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (cvImg.empty())
    {
        std::cout << "image decode fail=" << imgPathFile << std::endl;
        return;
    }

    std::vector<int> pngParam;
    pngParam.push_back(cv::IMWRITE_PNG_COMPRESSION);
    pngParam.push_back(3);

    std::vector<unsigned char> encodeBuf;
    if (cv::imencode(".png", cvImg, encodeBuf, pngParam) == false)
    {
        std::cout << "image encode fail=" << imgPathFile << std::endl;
        return;
    }
                
    std::string savePathFile = savePath + "/" + fs::path(imgPathFile).stem().string() + ".png";
    rt = WriteFileFromBuf(savePathFile, encodeBuf.data(), encodeBuf.size());
    if (rt != 0)
    {
        std::cout << "image save fail=" << savePathFile << std::endl;
        return;
    }
}

// jpg 파일을 로드하고  png 포맷 데이터로 변환
void JpgToPngWorker(int index, const std::string& imgPathFile, ReorderBuf& reorderBuf)
{
    int rt = 0;
    std::vector<unsigned char> buf;

    rt = ReadFileToVecBuf(imgPathFile, buf);
    if (rt != 0)
    {
        std::cout << "image file load fail=" << imgPathFile << std::endl;
        reorderBuf.push(index, {});
        return;
    }

    cv::Mat cvImg = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (cvImg.empty())
    {
        std::cout << "image decode fail=" << imgPathFile << std::endl;
        reorderBuf.push(index, {});
        return;
    }

    std::vector<int> pngParam;
    pngParam.push_back(cv::IMWRITE_PNG_COMPRESSION);
    pngParam.push_back(3);

    std::vector<unsigned char> encodeBuf;
    if (cv::imencode(".png", cvImg, encodeBuf, pngParam) == false)
    {
        std::cout << "image encode fail=" << imgPathFile << std::endl;
        reorderBuf.push(index, {});
        return;
    }

    reorderBuf.push(index, std::move(encodeBuf));
}

// 파일 저장 스레드 함수
void SaveWorker(ReorderBuf& reorderBuf, int totalFile, const std::string& savePath, int& workCount)
{
    int rt = 0;
    char pathFile[260] = { '\0', };

    for (int i = 0; i < totalFile; i++)
    {
        std::vector<unsigned char> data = reorderBuf.pop_next();

        if (data.empty())
        {
            std::cout << "format convert fail" << std::endl;
            continue;
        }

        snprintf(pathFile, sizeof(pathFile), "%s/%03d.png", savePath.c_str(), i + 1);
        rt = WriteFileFromBuf(pathFile, data.data(), data.size());
        
        if (rt != 0) std::cout << "file save fail=" << pathFile << std::endl;
        else std::cout << "work count=" << ++workCount << std::endl;
    }
}

// 프로그램 사용법 안내
void printUsage(const char* progName) {
    std::cout << "사용법: " << progName << " [옵션]\n\n"
        << "옵션:\n"
        << "  -t, --type <정수>      작업 타입 지정. 0 - 순차처리, 1 - 병렬처리 \n"
        << "  -r, --repeat <정수>    반복 횟수 지정 \n"
        << "  -i, --input <경로>     입력 JPG 폴더 경로\n"
        << "  -o, --output <경로>    출력 PNG 저장 폴더 경로\n"
        << "  -h, --help             도움말 메시지 출력\n\n"
        << "예시:\n"
        << "  " << progName << " -t 1 -r 5 -i ./src -o ./dest\n";
}

// 사용자 입력 파싱
int CommandParsing(int argc, char* argv[], int& type, int& repeatCount, std::string& jpgPath, std::string& savePath)
{
    // 필수 옵션 입력 여부 확인용 플래그
    bool hasType = false;
    bool hasRepeat = false;
    bool hasInput = false;
    bool hasOutput = false;

    // 인자가 없으면 도움말 출력 후 종료
    if (argc <= 1) {
        printUsage(argv[0]);
        return -1;
    }

    for (int i = 1; i < argc; ++i) 
    {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return -2;
        }
        else if ((arg == "-t" || arg == "--type") && i + 1 < argc) {
            try {
                type = std::stoi(argv[++i]);
                hasType = true;
            }
            catch (const std::exception& e) {
                std::cerr << "error : integer must be entered for '-t' option" << e.what() << "\n";
                return -3;
            }
        }
        else if ((arg == "-r" || arg == "--repeat") && i + 1 < argc) {
            try {
                repeatCount = std::stoi(argv[++i]);
                hasRepeat = true;
            }
            catch (const std::exception& e) {
                std::cerr << "error : integer must be entered for '-r' option" << e.what() << "\n";
                return -4;
            }
        }
        else if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            jpgPath = fs::path(argv[++i]).generic_string();
            hasInput = true;
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            savePath = fs::path(argv[++i]).generic_string();
            hasOutput = true;
        }
        else {
            std::cerr << "error : unknown option or missing value\n";
            printUsage(argv[0]);
            return -5;
        }
    }

    // 필수 인자 누락 검증
    if (!hasType || !hasRepeat || !hasInput || !hasOutput) 
    {
        std::cerr << "error : required option missing\n\n";
        printUsage(argv[0]);
        return -6;
    }

    // 파싱된 옵션 확인
    std::cout << "[option]\n"
        << " - type: " << type << "\n"
        << " - repeat: " << repeatCount << "\n"
        << " - input: " << jpgPath << "\n"
        << " - output: " << savePath << "\n\n";

    return 0;
}

MemMonitor g_memMonitor;     // 메모리 사용량 측정
static int g_workCount = 0;  // 작업량 표시

int main(int argc, char* argv[])
{
    int rt = 0;

    int type = 0;
    int repeatCount = 0;
    std::string jpgPath = "";
    std::string savePath = "";
    std::vector<std::string> jpgFileList;
    
    rt = CommandParsing(argc, argv, type, repeatCount, jpgPath, savePath);
    if (rt != 0) return rt;

    g_memMonitor.start(600, true, "./mem.csv");

    jpgFileList = ListFileWithExt(jpgPath, ".jpg");
    if (!jpgFileList.empty())
    {
        std::vector<unsigned char> vBuf;
        std::vector<unsigned char> encodeBuf;
        cv::Mat cvImg;

        std::vector<int> pngParam;
        pngParam.push_back(cv::IMWRITE_PNG_COMPRESSION);
        pngParam.push_back(3);

        std::string savePathFile = "";
        fs::path pathObj("");

        if (!fs::exists(savePath)) fs::create_directories(savePath);

        auto start = std::chrono::steady_clock::now();

        if (type == 0)
        {
            // 순차 처리
            for (int i = 0; i < repeatCount; i++)
            {
                for (const std::string& jpgPathFile : jpgFileList)
                {
                    rt = ReadFileToVecBuf(jpgPathFile, vBuf);
                    if (rt != 0)
                    {
                        std::cout << "file load fail=" << jpgPathFile << std::endl;
                        continue;
                    }

                    cvImg = cv::imdecode(vBuf, cv::IMREAD_COLOR);
                    if (cvImg.empty())
                    {
                        std::cout << "image data empty=" << jpgPathFile << std::endl;
                        continue;
                    }

                    if (cv::imencode(".png", cvImg, encodeBuf, pngParam) == false)
                    {
                        std::cout << "image encode fail=" << jpgPathFile << std::endl;
                        continue;
                    }

                    savePathFile = savePath + "/" + pathObj.assign(jpgPathFile).stem().string() + ".png";
                    rt = WriteFileFromBuf(savePathFile, encodeBuf.data(), encodeBuf.size());

                    if (rt == 0) std::cout << "work count=" << ++g_workCount << std::endl;
                }
            }
        }
        else if (type == 1)
        {
            // 병렬 처리
			for (int i = 0; i < repeatCount; i++)
			{
				int totalFile = (int)jpgFileList.size();

				// 스레드 풀 생성
				ThreadPool pool(4);

				// 재정렬 버퍼 생성 - 입력 순서에 따른 출력 보장
				ReorderBuf reorderBuf;

				// 이미지 파일 저장 스레드 실행
				std::thread writer(SaveWorker, std::ref(reorderBuf), totalFile, std::cref(savePath), std::ref(g_workCount));

				// 스레드 풀에 이미지 변환 작업 할당
				for (int i = 0; i < totalFile; i++)
				{
					std::string pathFile = jpgFileList[i];

					// 스레드 풀에 작업 할당 - 현재 파일의 인덱스를 같이 넘김
					pool.enqueue([i, pathFile, &reorderBuf]()
						{
							JpgToPngWorker(i, pathFile, reorderBuf);
						});
				}

				// 스레드 종료 대기
				if (writer.joinable()) writer.join();
			}
        }

        auto end = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::milli> duration = end - start;
        std::cout << "time spent for task : " << duration.count() << " ms" << std::endl;
        
        g_memMonitor.stop();
    }

   return 0;
}