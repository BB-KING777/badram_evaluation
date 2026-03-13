#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>  // 空きメモリ容量を取得するため
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>         // 実行時間の計測用

#define PAGE_SIZE 4096       // 一般的なページサイズ（4KB）
#define MAX_ERRORS 10000     // 記録する最大エラー数。さすがに1万個もエラーが出たらシステムが死んでるはず…
#define CSV_FILENAME "evaluation.csv" // 結果の保存先

// --- テストの実行条件 ---
// 95%から1%まで、1%刻みで各5回ずつテストを回す設定。
// 結構時間がかかるので、急ぎの時はステップ幅を大きくするかも。
#define NUM_TRIALS 5         // 同じパーセンテージでの試行回数
#define PERCENTAGE_STEP 1    // 何%ずつ減らしていくか
#define START_PERCENTAGE 95  // 最初はギリギリまで攻める（95%）
#define END_PERCENTAGE 1     // 最終ライン

// エラーが起きた箇所を記録するための構造体
typedef struct {
    void *actual_addr; // 実際に書き込んだアドレス
    void *read_addr;   // 読み出して化けてしまっていた謎のアドレス
} ErrorRecord;

// 1回の試行結果をまとめておく構造体
// CSVに書き出すときに使う
typedef struct {
    const char *result;     // "OK" か "NG" か、あるいは "ALLOC_FAIL"
    int error_count;        // 見つかったエラーの数
    int reverse_errors;     // 逆方向のテストで一致してしまった数（エイリアス等の確認用）
    size_t allocated_mb;    // 確保できたメモリ量(MB)
    size_t allocated_pages; // 確保できたページ数
    size_t free_mb;         // テスト開始時のシステムの空きメモリ(MB)
    double exec_time;       // 実行にかかった時間(秒)
} TrialResult;

#define CACHE_LINE_SIZE 64 // x86の一般的なキャッシュラインサイズ

/*
 * 指定したアドレスのキャッシュを強制的にフラッシュする関数
 * これをやらないと、CPUのキャッシュにヒットしてしまって
 * 実際の物理メモリ（RAM）のテストにならないので超重要！
 */
static inline void flush_page(void *addr) {
    char *ptr = (char *)addr;
    
    // 1ページ(4096バイト)分、キャッシュライン(64バイト)ごとにフラッシュしていく
    for (size_t i = 0; i < PAGE_SIZE; i += CACHE_LINE_SIZE) {
        // clflush命令を使って明示的にキャッシュから追い出す
        asm volatile("clflush (%0)" : : "r"(ptr + i) : "memory");
    }
}

/*
 * メモリバリア
 * コンパイラやCPUが勝手に命令の実行順序を並べ替える（アウトオブオーダ実行）のを防ぐ。
 * 書き込みが終わる前に読み込みが走ったりするとパニックになるため。
 */
static inline void memory_barrier(void) {
    asm volatile("mfence" ::: "memory");
}

/*
 * 実行結果をCSVファイルに追記する関数
 * 毎回ファイルを開いて閉じてるのはちょっと遅いかもしれないけど、
 * 途中でプログラムがクラッシュした時でもそれまでの結果を残したいので、あえてこうしている。
 */
void append_result(int percentage, int trial, const char *result, 
                   int error_count, int reverse_errors,
                   size_t allocated_mb, size_t allocated_pages,
                   size_t free_mb, double exec_time) {
    FILE *fp = fopen(CSV_FILENAME, "a"); // 追記モードで開く
    if (fp == NULL) {
        perror("Failed to open CSV file");
        return; // 開けなくてもプログラム自体は止めない
    }
    
    // データをカンマ区切りで書き込み
    fprintf(fp, "%d,%d,%s,%d,%d,%zu,%zu,%zu,%.2f\n",
            percentage, trial, result, error_count, reverse_errors,
            allocated_mb, allocated_pages, free_mb, exec_time);
    
    fclose(fp);
}

/*
 * CSVのヘッダ（1行目）を作る関数
 * 既にファイルが存在していれば何もしない。
 */
void create_csv_header() {
    FILE *fp = fopen(CSV_FILENAME, "r");
    if (fp != NULL) {
        // 既にファイルがあるなら、そのまま閉じて終わる
        fclose(fp);
        return;
    }
    
    // ファイルが存在しない場合は新規作成
    fp = fopen(CSV_FILENAME, "w");
    if (fp == NULL) {
        perror("Failed to create CSV file");
        exit(1); // ここでコケたらさすがに終了する
    }
    
    // ヘッダ行を書き込み
    fprintf(fp, "percentage,trial,result,error_count,reverse_errors,allocated_mb,allocated_pages,free_memory_mb,execution_time_sec\n");
    fclose(fp);
}

/*
 * CSVを読み込んで前回の進捗を確認する関数
 * 途中でプロセスが死んでも続きから再開できる神機能。
 * 戻り値: 1=レジューム再開, 0=最初から
 */
int read_progress(int *start_pct, int *start_trial) {
    FILE *fp = fopen(CSV_FILENAME, "r");
    if (fp == NULL) {
        // ファイルがないなら当然最初から！
        *start_pct = START_PERCENTAGE;
        *start_trial = 1;
        return 0;
    }

    char line[512];
    int last_pct = START_PERCENTAGE;
    int last_trial = 0;

    // 1行目（ヘッダ）は読み飛ばす
    fgets(line, sizeof(line), fp);

    // ほんとは fseek で末尾から逆読みした方がスマートだけど、
    // まぁ数千行程度なら先頭から全部舐めても一瞬なので愚直にいく。
    while (fgets(line, sizeof(line), fp) != NULL) {
        int pct, trial;
        // 行の先頭から percentage と trial を強引に引っこ抜く
        if (sscanf(line, "%d,%d", &pct, &trial) == 2) {
            last_pct = pct;
            last_trial = trial; // 最後の行の値で上書きし続ける
        }
    }
    fclose(fp);

    // 読み取った「最後の記録」から、次にやるべきステップを計算する
    if (last_trial < NUM_TRIALS) {
        // まだそのパーセンテージの試行回数が残ってるなら、続きから
        *start_pct = last_pct;
        *start_trial = last_trial + 1;
    } else {
        // そのパーセンテージが全部終わってたなら、次のパーセンテージの1回目から
        *start_pct = last_pct - PERCENTAGE_STEP;
        *start_trial = 1;
    }

    // 既に最終ラインまで終わってたら、これ以上やることなし！
    if (*start_pct < END_PERCENTAGE) {
        printf("全てのテストは既に完了しているようです！お疲れ様でした！\n");
        exit(0);
    }

    return 1;
}

/*
 * 1回分のメモリテストを実行し、結果を TrialResult に格納する。
 * 戻り値: 0=正常終了(エラー有無は問わず), -1=テスト自体が失敗(メモリ確保エラーなど)
 */
int run_single_trial(int percentage, int trial, TrialResult *result) {
    clock_t start_time = clock(); // 時間計測スタート
    
    printf("[%d%% #%d] ", percentage, trial);
    fflush(stdout); // 改行がないので、ここで画面に出力を強制的に反映させる
    
    // まずはシステムの現在の空きメモリ状況を取得する
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        perror("sysinfo failed");
        return -1;
    }
    
    // sysinfoから空きメモリ容量（バイト数）を計算
    size_t free_memory = info.freeram * info.mem_unit;
    // 指定されたパーセンテージ分のサイズを計算
    size_t alloc_size = (free_memory * percentage) / 100;
    // ページサイズ(4KB)の倍数になるように調整する
    size_t num_pages = alloc_size / PAGE_SIZE;
    alloc_size = num_pages * PAGE_SIZE;
    
    // CSV出力用のデータを構造体にセットしておく
    result->free_mb = free_memory / (1024 * 1024);
    result->allocated_mb = alloc_size / (1024 * 1024);
    result->allocated_pages = num_pages;
    
    // メモリの確保！
    // ほんとは mmap+madvise とかで物理メモリに直接アプローチした方が
    // 確実かもしれないけど、とりあえず手軽な malloc で様子見する。
    // （OSのオーバーコミット設定によっては、実際に触るまで物理メモリが割り当てられないので注意）
    void *memory = malloc(alloc_size);
    if (memory == NULL) {
        printf("ALLOC_FAIL\n"); // 確保できなかったら潔く諦める
        result->result = "ALLOC_FAIL";
        result->error_count = 0;
        result->reverse_errors = 0;
        result->exec_time = 0.0;
        return -1;
    }

    // エラー情報を記録するための配列も確保
    ErrorRecord *errors = malloc(MAX_ERRORS * sizeof(ErrorRecord));
    if (errors == NULL) {
        printf("ERROR_ARRAY_FAIL\n");
        free(memory); // 確保したメインメモリを忘れずに解放
        return -1;
    }
    
    // 【フェーズ1：書き込み】
    // 確保した領域の各ページの先頭に、そのページ自身のアドレスを書き込んでいく
    for (size_t i = 0; i < num_pages; i++) {
        void **page = (void **)((char *)memory + i * PAGE_SIZE);
        *page = page; // ポインタの先に自分のアドレスを格納
        flush_page(page); // すかさずキャッシュをフラッシュしてRAMへ追い出す！
    }
    // 全部の書き込みが確実に終わるまでここで待機
    memory_barrier();
    
    // 【フェーズ2：スキャン（読み取りチェック）】
    int error_count = 0;
    for (size_t i = 0; i < num_pages; i++) {
        void **page = (void **)((char *)memory + i * PAGE_SIZE);
        
        // 読み込む前にもう一度フラッシュして、確実にRAMから読ませる
        flush_page(page);
        memory_barrier();
        
        void *stored_addr = *page; // 実際に読み出した値
        
        // 書き込んだはずのアドレスと違っていたら…エラー発生！
        if (stored_addr != page) {
            // 用意した配列の限界までは記録しておく
            if (error_count < MAX_ERRORS) {
                errors[error_count].actual_addr = page;
                errors[error_count].read_addr = stored_addr;
                error_count++;
            }
        }
    }
    
    // 【フェーズ3：逆方向テスト（エラーがあった場合のみ）】
    // エラー（違うアドレスに化けていた）があった場合、
    // 化けていた先のアドレスに書き込んだら、元のアドレスの値も変わるのか？
    // をチェックする。
    int reverse_errors = 0;
    if (error_count > 0) {
        const char *test_pattern = "Is This BadRAM?"; // わかりやすい適当な文字列
        size_t pattern_len = strlen(test_pattern) + 1;
        
        for (int i = 0; i < error_count; i++) {
            char *actual = (char *)errors[i].actual_addr;
            char *read = (char *)errors[i].read_addr;

            // 化けていたアドレスが、今回確保したメモリ領域の外だったら
            // セグフォで落ちる危険があるのでスキップする。
            if ((void*)read < memory || (void*)read >= (memory + alloc_size)) {
                continue;
            }
            
            // 化け先のアドレスに文字列を書き込む
            memcpy(actual, test_pattern, pattern_len);
            flush_page(actual);
            memory_barrier();
            
            // 元のアドレスから読み出してみる
            flush_page(read);
            memory_barrier();
            
            // もし書き込んだはずのない元のアドレスから文字列が出てきたら、
            // 内部で同じ物理メモリを参照してしまっている（エイリアス）証拠。
            if (memcmp(read, test_pattern, pattern_len) == 0) {
                reverse_errors++;
            }
        }
    }
    
    // タイム計測終了
    clock_t end_time = clock();
    double exec_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    
    // 結果を構造体にまとめる
    result->result = (error_count > 0) ? "NG" : "OK";
    result->error_count = error_count;
    result->reverse_errors = reverse_errors;
    result->exec_time = exec_time;
    
    // 画面にもサクッと結果を表示
    if (error_count > 0) {
        printf("NG err=%d rev=%d %.1fs\n", error_count, reverse_errors, exec_time);
    } else {
        printf("OK %.1fs\n", exec_time);
    }
    
    // お片付け。これを忘れるとメモリリークして大変なことになる
    free(errors);
    free(memory);
    
    return 0; // 無事に1回のテスト完了
}

int main() {
    // 起動時のメッセージ。設定値を表示しておく。
    printf("Memory Test: %d%%-%d%%, %d trials, step %d%%\n", 
           START_PERCENTAGE, END_PERCENTAGE, NUM_TRIALS, PERCENTAGE_STEP);
    
    // まずはCSVのヘッダを準備する（既にあれば何もしない）
    create_csv_header();
    
    // --- 進捗の読み取り ---
    int start_pct, start_trial;
    if (read_progress(&start_pct, &start_trial)) {
        printf("\n*** レジューム機能発動！前回の中断箇所（%d%% - %d回目）から再開します ***\n", start_pct, start_trial);
    } else {
        printf("\n*** 新規テストを開始します（%d%% - 1回目から） ***\n", START_PERCENTAGE);
    }
    
    // 指定したパーセンテージ（例：95%から1%まで）を下げながらループ
    // レジューム時は計算された start_pct から始まる！
    for (int percentage = start_pct; percentage >= END_PERCENTAGE; percentage -= PERCENTAGE_STEP) {
        printf("\n=== %d%% ===\n", percentage);
        
        // trial の開始位置の決定。
        // レジュームで再開した最初のパーセンテージの時だけ、途中の回数（start_trial）から始める。
        // 次のパーセンテージに進んだら、当然1回目からやり直す。
        int current_start_trial = (percentage == start_pct) ? start_trial : 1;
        
        for (int trial = current_start_trial; trial <= NUM_TRIALS; trial++) {
            TrialResult result;
            
            // テスト実行！
            if (run_single_trial(percentage, trial, &result) == 0) {
                // 成功裏に終わったら（NGでもOKでも）CSVに記録
                append_result(percentage, trial, result.result,
                              result.error_count, result.reverse_errors,
                              result.allocated_mb, result.allocated_pages,
                              result.free_mb, result.exec_time);
            } else {
                // どこで限界が来たかを知るために記録は残しておく。
                // 確保量が0なので、それ以外の数字は一旦0埋め。
                append_result(percentage, trial, result.result,
                              0, 0, 0, 0, 0, 0.0);
            }
        }
    }
    
    // 終わりのコメント
    printf("\nDone! Results: %s\n", CSV_FILENAME);
    
    return 0;
}