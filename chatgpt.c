#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define DEFAULT_MODEL "gpt-4o-mini"

/* Renkler */
#define COLOR_RESET   "\033[0m"
#define COLOR_USER    "\033[1;36m"
#define COLOR_ASSIST  "\033[1;32m"
#define COLOR_INFO    "\033[0;90m"
#define COLOR_ERROR   "\033[1;31m"
#define COLOR_CMD     "\033[1;35m"

/* Konuşma geçmişi */
#define MAX_TURNS 100

typedef struct {
    char *user;
    char *assistant;
} Turn;

static Turn TURNS[MAX_TURNS];
static int TURN_COUNT = 0;

/* Komut listesi */
#define MAX_CMDS 16
static char *LAST_CMDS[MAX_CMDS];
static int LAST_CMD_COUNT = 0;

/* Log kontrolü */
static int QUIET_MODE = 0;

/* Dil ayarı */
static char *CURRENT_LANG = NULL;

/* curl için bellek */
struct Memory {
    char *data;
    size_t size;
};

/* ===== Yardımcılar ===== */

static void log_msg(const char *msg) {
    if (!QUIET_MODE) {
        fprintf(stderr, "[chatgpt-cli] %s\n", msg);
    }
}

static char *my_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    size_t capacity = 4096;
    size_t len = 0;
    char *buf = malloc(capacity);
    if (!buf) { fclose(f); return NULL; }

    size_t r;
    while ((r = fread(buf + len, 1, capacity - len - 1, f)) > 0) {
        len += r;
        if (len >= capacity - 1) {
            capacity *= 2;
            char *tmp = realloc(buf, capacity);
            if (!tmp) { free(buf); fclose(f); return NULL; }
            buf = tmp;
        }
    }
    
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(data, f) == EOF) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t')) {
        s[len-1] = '\0';
        len--;
    }
}

/* JSON escape: sadece ", \, \n, \r, \t için basit kaçış */
static char *json_escape(const char *src) {
    size_t len = strlen(src);
    size_t max_len = len * 2 + 16;
    char *out = malloc(max_len);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
        } else {
            out[j++] = (char)c;
        }
        if (j + 2 >= max_len) {
            max_len *= 2;
            char *tmp = realloc(out, max_len);
            if (!tmp) {
                free(out);
                return NULL;
            }
            out = tmp;
        }
    }
    out[j] = '\0';
    return out;
}

/* curl callback */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        log_msg("Bellek hatası (realloc).");
        return 0;
    }

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

/* Basit Unicode \uXXXX decode (UTF-8'e çevirir) */
static int decode_unicode(const char *p, char *out) {
    unsigned int code;
    if (sscanf(p, "%4x", &code) != 1) return 0;

    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    } else if (code < 0x800) {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
}

/* JSON içinden assistant content çekme (Geliştirilmiş) */
static char *extract_assistant_content(const char *json) {
    /* Önce hata kontrolü */
    if (strstr(json, "\"error\":")) {
        log_msg("API Hatası tespit edildi:");
        fprintf(stderr, "%s\n", json);
        return NULL;
    }

    const char *role_pos = strstr(json, "\"role\":\"assistant\"");
    const char *start_search = json;

    if (role_pos) {
        start_search = role_pos;
    }

    const char *cpos = strstr(start_search, "\"content\":");
    if (!cpos) {
        log_msg("JSON içinde content alanı bulunamadı. Ham cevap:");
        fprintf(stderr, "%s\n", json);
        return NULL;
    }

    cpos += strlen("\"content\":");

    while (*cpos == ' ' || *cpos == '\t' || *cpos == '\n' || *cpos == '\r') {
        cpos++;
    }

    if (*cpos != '\"') {
        log_msg("content alanı beklenen formatta değil. Ham cevap:");
        fprintf(stderr, "%s\n", json);
        return NULL;
    }

    cpos++;

    size_t max_len = strlen(cpos) + 1;
    char *out = malloc(max_len);
    if (!out) return NULL;

    size_t j = 0;
    int escape = 0;

    for (const char *p = cpos; *p; p++) {
        char c = *p;
        if (escape) {
            if (c == 'u') {
                /* Unicode decode */
                int len = decode_unicode(p + 1, &out[j]);
                if (len > 0) {
                    j += len;
                    p += 4; /* \uXXXX -> 4 hex digits + 'u' handled by loop logic? No. */
                    /* p is at 'u'. p+1 is start of hex. decode reads 4 chars. */
                    /* We need to skip 'u' and 4 hex chars. Total 5 chars consumed from p. */
                    /* Loop increments p by 1. So we add 4 to p. */
                } else {
                    /* Decode failed, keep raw */
                    out[j++] = 'u';
                }
            } else if (c == 'n') out[j++] = '\n';
            else if (c == 'r') out[j++] = '\r';
            else if (c == 't') out[j++] = '\t';
            else if (c == '\"') out[j++] = '\"';
            else if (c == '\\') out[j++] = '\\';
            else out[j++] = c;
            
            escape = 0;
        } else {
            if (c == '\\') {
                escape = 1;
            } else if (c == '\"') {
                break;
            } else {
                out[j++] = c;
            }
        }
        if (j + 4 >= max_len) { /* +4 for unicode expansion safety */
            max_len *= 2;
            char *tmp = realloc(out, max_len);
            if (!tmp) {
                free(out);
                return NULL;
            }
            out = tmp;
        }
    }

    out[j] = '\0';
    return out;
}

/* Config yolu */
static char *get_config_path(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    size_t len = strlen(home) + strlen("/.config/chatgpt-cli-c/config") + 1;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/.config/chatgpt-cli-c/config", home);
    return path;
}

/* API key yükleme */
static char *load_api_key(void) {
    const char *env_key = getenv("OPENAI_API_KEY");
    if (env_key && env_key[0] != '\0') {
        log_msg("API anahtarı OPENAI_API_KEY ortam değişkeninden yüklendi.");
        return my_strdup(env_key);
    }

    char *config_path = get_config_path();
    if (!config_path) {
        log_msg("Config yolu oluşturulamadı.");
        return NULL;
    }

    char *content = read_file(config_path);
    if (content) {
        trim(content);
        if (content[0] != '\0') {
            log_msg("API anahtarı config dosyasından yüklendi.");
            free(config_path);
            return content;
        }
        free(content);
    }

    log_msg("Config dosyası yok veya boş. İlk kurulum.");
    printf("OpenAI API anahtarını gir (sadece ilk sefer): ");
    fflush(stdout);

    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) {
        log_msg("API anahtarı okunamadı.");
        free(config_path);
        return NULL;
    }
    trim(buf);
    if (buf[0] == '\0') {
        log_msg("Boş API anahtarı girildi.");
        free(config_path);
        return NULL;
    }

    const char *home = getenv("HOME");
    if (!home) home = ".";
    char dirpath[1024];
    snprintf(dirpath, sizeof(dirpath), "%s/.config/chatgpt-cli-c", home);
    if (mkdir(dirpath, 0700) != 0 && errno != EEXIST) {
        perror("mkdir");
        log_msg("Config dizini oluşturulamadı.");
        free(config_path);
        return NULL;
    }

    if (write_file(config_path, buf) != 0) {
        log_msg("Config dosyasına yazılamadı.");
        free(config_path);
        return NULL;
    }

    chmod(config_path, 0600);
    log_msg("API anahtarı config dosyasına kaydedildi (~/.config/chatgpt-cli-c/config).");
    free(config_path);
    return my_strdup(buf);
}

/* Config'ten varsayılan model okuma / yazma */
static char *get_model_from_config(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char path[1024];
    snprintf(path, sizeof(path), "%s/.config/chatgpt-cli-c/model", home);

    char *content = read_file(path);
    if (!content) return NULL;

    trim(content);
    return content;
}

static int write_model_to_config(const char *model) {
    const char *home = getenv("HOME");
    if (!home) return -1;

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.config/chatgpt-cli-c", home);
    mkdir(dir, 0700);

    char path[1024];
    snprintf(path, sizeof(path), "%s/model", dir);

    if (write_file(path, model) != 0) {
        return -1;
    }
    chmod(path, 0600);
    return 0;
}

/* Dil ayarları okuma / yazma */
static char *get_lang_path(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    size_t len = strlen(home) + strlen("/.config/chatgpt-cli-c/lang") + 1;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/.config/chatgpt-cli-c/lang", home);
    return path;
}

static char *get_lang_from_config(void) {
    char *path = get_lang_path();
    if (!path) return NULL;
    char *content = read_file(path);
    free(path);
    if (content) {
        trim(content);
        if (content[0] == '\0') {
            free(content);
            return NULL;
        }
    }
    return content;
}

static int write_lang_to_config(const char *lang) {
    char *path = get_lang_path();
    if (!path) return -1;
    
    const char *home = getenv("HOME");
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.config/chatgpt-cli-c", home ? home : ".");
    mkdir(dir, 0700);

    int res = write_file(path, lang);
    if (res == 0) chmod(path, 0600);
    free(path);
    return res;
}

/* Komut listesi yönetimi */
static void clear_last_cmds(void) {
    for (int i = 0; i < LAST_CMD_COUNT; i++) {
        free(LAST_CMDS[i]);
        LAST_CMDS[i] = NULL;
    }
    LAST_CMD_COUNT = 0;
}

static void extract_commands_from_answer(const char *answer) {
    clear_last_cmds();
    if (!answer) return;

    const char *p = answer;
    while (*p && LAST_CMD_COUNT < MAX_CMDS) {
        const char *line_start = p;
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        if (len >= 3 && line_start[0] == '$' && line_start[1] == ' ') {
            size_t cmdlen = len - 2;
            char *cmd = malloc(cmdlen + 1);
            if (cmd) {
                memcpy(cmd, line_start + 2, cmdlen);
                cmd[cmdlen] = '\0';
                LAST_CMDS[LAST_CMD_COUNT++] = cmd;
            }
        }

        if (!line_end) break;
        p = line_end + 1;
    }
}

/* Konuşma geçmişi */
static void add_turn(const char *user, const char *assistant) {
    if (!user || !assistant) return;

    if (TURN_COUNT >= MAX_TURNS) {
        free(TURNS[0].user);
        free(TURNS[0].assistant);
        memmove(&TURNS[0], &TURNS[1], sizeof(Turn) * (MAX_TURNS - 1));
        TURN_COUNT = MAX_TURNS - 1;
    }

    TURNS[TURN_COUNT].user = my_strdup(user);
    TURNS[TURN_COUNT].assistant = my_strdup(assistant);
    TURN_COUNT++;
}

static void print_history(void) {
    if (TURN_COUNT == 0) {
        printf("%s(henüz geçmiş yok)%s\n", COLOR_INFO, COLOR_RESET);
        return;
    }

    for (int i = 0; i < TURN_COUNT; i++) {
        printf("%s[%d] Ben:%s %s\n",
               COLOR_USER, i + 1, COLOR_RESET,
               TURNS[i].user ? TURNS[i].user : "");
        printf("%s[%d] ChatGPT:%s\n%s\n",
               COLOR_ASSIST, i + 1, COLOR_RESET,
               TURNS[i].assistant ? TURNS[i].assistant : "");
        printf("----\n");
    }
}

/* Çok satırlı giriş */
static char *read_multiline_prompt(void) {
    printf("%sÇok satırlı moda geçtin. Metni yaz, sadece '.' içeren bir satırla bitir.%s\n",
           COLOR_INFO, COLOR_RESET);

    size_t cap = 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        printf("%sBellek hatası.%s\n", COLOR_ERROR, COLOR_RESET);
        return NULL;
    }
    buf[0] = '\0';

    while (1) {
        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        trim(line);
        if (strcmp(line, ".") == 0) {
            break;
        }
        size_t l = strlen(line);
        if (len + l + 2 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                printf("%sBellek hatası.%s\n", COLOR_ERROR, COLOR_RESET);
                return NULL;
            }
            buf = tmp;
        }
        memcpy(buf + len, line, l);
        len += l;
        buf[len++] = '\n';
        buf[len] = '\0';
    }

    if (len == 0) {
        free(buf);
        printf("%sBoş çok satırlı giriş.%s\n", COLOR_INFO, COLOR_RESET);
        return NULL;
    }

    return buf;
}

/* Argümanları birleştirme */
static char *join_args_from(int start, int argc, char **argv) {
    size_t total = 0;
    for (int i = start; i < argc; i++) {
        total += strlen(argv[i]) + 1;
    }
    char *out = malloc(total + 1);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = start; i < argc; i++) {
        strcat(out, argv[i]);
        if (i != argc - 1) strcat(out, " ");
    }
    return out;
}

/* CLI yardım ve model listesi */
static void print_usage(const char *progname) {
    fprintf(stdout,
        "Kullanim: %s [seçenekler] [\"tek seferlik soru...\"]\n\n"
        "Seçenekler:\n"
        "  -m, --model ADI            Bu istek için model seç\n"
        "  --set-default-model ADI    Varsayılan modeli kalıcı olarak ayarla\n"
        "  -l, --list-models          Kullanılabilir modelleri listele\n"
        "  -q, --no-log               Sessiz mod (logları kapat)\n"
        "  -h, --help                 Bu yardımı göster\n\n"
        "Model önceliği: CLI > config > CHATGPT_MODEL > DEFAULT_MODEL\n\n"
        "Etkileşimli mod komutları:\n"
        "  /history    Konuşma geçmişini göster\n"
        "  /clear      Konuşma geçmişini temizle\n"
        "  /ml         Çok satırlı mesaj yaz ('.' ile bitir)\n"
        "  /run N      Son yanıttaki $ komutlarından N'inciyi çalıştır\n"
        "  /model      Aktif modeli göster\n"
        "  /exit       Çıkış\n",
        progname
    );
}

static void print_models(void) {
    fprintf(stdout,
        "Örnek sohbet modelleri:\n\n"
        "  gpt-4o-mini   - Hızlı, ucuz, günlük işler\n"
        "  gpt-4.1-mini  - Mini serisinin yeni nesli (erişimin varsa)\n"
        "  gpt-4o        - Daha güçlü, multimodal, genel amaçlı\n"
        "  gpt-4.1       - Güçlü, teknik işler ve kod için iyi\n"
        "  gpt-4.1-pro   - En üst seviye, yoğun reasoning için\n"
        "  o3-mini       - Mantık / reasoning odaklı\n\n"
        "Not: Hesabında hangilerinin açık olduğunu OpenAI panelinden kontrol et.\n"
    );
}

/* Dinamik String Buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
}

static void sb_append(StrBuf *sb, const char *s) {
    if (!sb->data || !s) return;
    size_t l = strlen(s);
    while (sb->len + l + 1 >= sb->cap) {
        sb->cap *= 2;
        char *tmp = realloc(sb->data, sb->cap);
        if (!tmp) return;
        sb->data = tmp;
    }
    memcpy(sb->data + sb->len, s, l);
    sb->len += l;
    sb->data[sb->len] = '\0';
}

static void sb_free(StrBuf *sb) {
    if (sb->data) free(sb->data);
}

/* OpenAI çağrısı */
static char *call_openai(const char *api_key, const char *model, const char *prompt) {
    CURL *curl;
    CURLcode res;
    struct Memory chunk;
    struct curl_slist *headers = NULL;

    chunk.data = malloc(1);
    chunk.size = 0;
    if (!chunk.data) {
        log_msg("Bellek hatası (chunk).");
        return NULL;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        log_msg("curl_easy_init başarısız.");
        free(chunk.data);
        curl_global_cleanup();
        return NULL;
    }

    /* JSON Payload Oluşturma (Geçmiş dahil) */
    StrBuf sb;
    sb_init(&sb);
    
    sb_append(&sb, "{");
    sb_append(&sb, "\"model\":\"");
    sb_append(&sb, model);
    sb_append(&sb, "\",\"messages\":[");

    /* System Prompt */
    const char *sys_text = "Sen Linux terminalinden erişilen yardımcı bir asistansın. Türkçe konuş.";
    if (CURRENT_LANG && strcmp(CURRENT_LANG, "en") == 0) {
        sys_text = "You are a helpful assistant accessed from a Linux terminal.";
    }
    
    char *esc_sys = json_escape(sys_text);
    if (esc_sys) {
        sb_append(&sb, "{\"role\":\"system\",\"content\":\"");
        sb_append(&sb, esc_sys);
        sb_append(&sb, "\"},");
        free(esc_sys);
    }

    /* Geçmiş Mesajlar */
    for (int i = 0; i < TURN_COUNT; i++) {
        char *u = json_escape(TURNS[i].user);
        char *a = json_escape(TURNS[i].assistant);
        
        if (u) {
            sb_append(&sb, "{\"role\":\"user\",\"content\":\"");
            sb_append(&sb, u);
            sb_append(&sb, "\"},");
            free(u);
        }
        if (a) {
            sb_append(&sb, "{\"role\":\"assistant\",\"content\":\"");
            sb_append(&sb, a);
            sb_append(&sb, "\"},");
            free(a);
        }
    }

    /* Yeni Mesaj */
    char *esc_user = json_escape(prompt);
    if (esc_user) {
        sb_append(&sb, "{\"role\":\"user\",\"content\":\"");
        sb_append(&sb, esc_user);
        sb_append(&sb, "\"}");
        free(esc_user);
    }

    sb_append(&sb, "],\"temperature\":0.3}");

    char *payload = sb.data;
    if (!payload) {
        log_msg("Payload oluşturulamadı.");
        free(chunk.data);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return NULL;
    }

    log_msg("API isteği hazırlanıyor (Geçmiş dahil)...");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.openai.com/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); /* 60 saniye zaman aşımı */
    
    /* SSL Güvenliği */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    log_msg("API isteği gönderiliyor...");
    res = curl_easy_perform(curl);
    
    sb_free(&sb); /* Payload artık gerekli değil */

    if (res != CURLE_OK) {
        fprintf(stderr, "%schatgpt-cli: curl hatası:%s %s\n",
                COLOR_ERROR, COLOR_RESET, curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        free(chunk.data);
        return NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    log_msg("API cevabı alındı, parse ediliyor...");
    char *assistant_text = extract_assistant_content(chunk.data);
    free(chunk.data);
    return assistant_text;
}

/* ===== main ===== */

int main(int argc, char **argv) {
    char *api_key = load_api_key();
    if (!api_key) {
        log_msg("API anahtarı alınamadı, çıkılıyor.");
        return 1;
    }

    const char *model = DEFAULT_MODEL;
    const char *model_env = getenv("CHATGPT_MODEL");
    char *model_cfg = get_model_from_config();

    if (model_cfg && model_cfg[0]) {
        model = model_cfg;
    } else if (model_env && model_env[0]) {
        model = model_env;
    }

    const char *model_cli = NULL;
    int list_models_flag = 0;
    int set_default_model_flag = 0;
    const char *new_default_model = NULL;
    int first_non_option = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Hata: -m/--model bir model adı ister.\n");
                return 1;
            }
            model_cli = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--set-default-model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Hata: --set-default-model bir model adı ister.\n");
                return 1;
            }
            set_default_model_flag = 1;
            new_default_model = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list-models") == 0) {
            list_models_flag = 1;
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--no-log") == 0) {
            QUIET_MODE = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            free(api_key);
            free(model_cfg);
            return 0;
        } else {
            first_non_option = i;
            break;
        }
    }

    if (model_cli) {
        model = model_cli;
    }

    if (set_default_model_flag) {
        if (write_model_to_config(new_default_model) == 0) {
            printf("Varsayılan model '%s' olarak ayarlandı.\n", new_default_model);
        } else {
            printf("Varsayılan model kaydedilemedi.\n");
        }
        free(api_key);
        free(model_cfg);
        return 0;
    }

    if (list_models_flag) {
        print_models();
        free(api_key);
        free(model_cfg);
        return 0;
    }

    /* Dil ayarını yükle (hem tek seferlik hem etkileşimli mod için) */
    CURRENT_LANG = get_lang_from_config();
    /* Eğer config yoksa, tek seferlik modda varsayılan (NULL -> TR) kalır. 
       Etkileşimli modda aşağıda sorulacak. */

    /* Tek seferlik mod */
    if (first_non_option < argc) {
        char *prompt = join_args_from(first_non_option, argc, argv);
        if (!prompt) {
            log_msg("Argümanlar birleştirilirken bellek hatası.");
            free(api_key);
            free(model_cfg);
            return 1;
        }
        
        /* Estetik ve Sade Görünüm (Minimalist) */
        /* Logları geçici olarak susturuyoruz */
        int old_quiet = QUIET_MODE;
        QUIET_MODE = 1;

        printf("\n%s➤ Soru:%s %s\n", COLOR_USER, COLOR_RESET, prompt);
        
        /* Bekleme efekti yerine basitçe işlem yapıldığını belirtelim ama log basmayalım */
        /* call_openai içindeki loglar QUIET_MODE=1 olduğu için basılmayacak */
        
        char *answer = call_openai(api_key, model, prompt);
        
        if (answer) {
            printf("\n%s➤ ChatGPT (%s):%s\n", COLOR_ASSIST, model, COLOR_RESET);
            printf("%s%s%s\n\n", COLOR_RESET, answer, COLOR_RESET);
            free(answer);
        } else {
            printf("\n%s[!] Cevap alınamadı veya hata oluştu.%s\n", COLOR_ERROR, COLOR_RESET);
        }
        
        QUIET_MODE = old_quiet;
        
        free(prompt);
        free(api_key);
        free(model_cfg);
        return 0;
    }

    /* Etkileşimli mod */
    log_msg("Etkileşimli mod başlatıldı.");

    /* Dil Seçimi (Eğer yüklenmediyse sor) */
    if (!CURRENT_LANG) {
        printf("\nDil seçiniz / Select language [tr/en] (Default: tr): ");
        char lbuf[64];
        if (fgets(lbuf, sizeof(lbuf), stdin)) {
            trim(lbuf);
            if (lbuf[0] == 'e' || lbuf[0] == 'E') {
                CURRENT_LANG = my_strdup("en");
            } else {
                CURRENT_LANG = my_strdup("tr");
            }
            
            printf("Seçim kaydedilsin mi? / Save choice permanently? [y/N]: ");
            if (fgets(lbuf, sizeof(lbuf), stdin)) {
                trim(lbuf);
                if (lbuf[0] == 'y' || lbuf[0] == 'Y') {
                    write_lang_to_config(CURRENT_LANG);
                    printf("Dil ayarı kaydedildi: %s\n", CURRENT_LANG);
                }
            }
        } else {
            CURRENT_LANG = my_strdup("tr");
        }
    }

    /* UI Metinleri */
    const char *ui_welcome = "ChatGPT CLI (C sürümü)";
    const char *ui_model = "Aktif model";
    const char *ui_lang = "Aktif dil";
    const char *ui_cmds = "Komutlar: /exit, /quit, /model, /history, /clear, /ml, /run N";
    const char *ui_me = "Ben";
    const char *ui_bye = "Görüşürüz 👋";
    const char *ui_hist_cleared = "Sohbet geçmişi temizlendi.";
    const char *ui_suggested = "Önerilen komutlar";
    const char *ui_run_hint = "(Çalıştırmak için /run NUMARA yazabilirsin.)";
    const char *ui_no_cmd = "Çalıştırılabilir komut yok (son yanıtta '$ ' satırı yok).";
    const char *ui_invalid_num = "Geçerli bir komut numarası gir";
    const char *ui_run_cmd = "Çalıştırılacak komut";
    const char *ui_confirm = "Onaylıyor musun? [y/N]: ";
    const char *ui_cancelled = "İptal edildi.";
    const char *ui_ret_code = "Komut dönüş kodu";

    if (CURRENT_LANG && strcmp(CURRENT_LANG, "en") == 0) {
        ui_welcome = "ChatGPT CLI (C version)";
        ui_model = "Active model";
        ui_lang = "Active language";
        ui_cmds = "Commands: /exit, /quit, /model, /history, /clear, /ml, /run N";
        ui_me = "Me";
        ui_bye = "Bye 👋";
        ui_hist_cleared = "Chat history cleared.";
        ui_suggested = "Suggested commands";
        ui_run_hint = "(Type /run NUMBER to execute.)";
        ui_no_cmd = "No executable commands found (no '$ ' lines in last response).";
        ui_invalid_num = "Enter a valid command number";
        ui_run_cmd = "Command to run";
        ui_confirm = "Do you approve? [y/N]: ";
        ui_cancelled = "Cancelled.";
        ui_ret_code = "Command return code";
    }

    printf("%s%s%s\n", COLOR_INFO, ui_welcome, COLOR_RESET);
    printf("%s: %s\n", ui_model, model);
    printf("%s: %s\n", ui_lang, CURRENT_LANG);
    printf("%s\n", ui_cmds);

    char *buf = malloc(4096);
    if (!buf) {
        log_msg("Ana döngü için bellek ayrılamadı.");
        free(api_key);
        free(model_cfg);
        return 1;
    }

    while (1) {
        printf("\n%s%s:%s ", COLOR_USER, ui_me, COLOR_RESET);
        fflush(stdout);

        if (!fgets(buf, 4096, stdin)) {
            printf("\nÇıkılıyor.\n");
            break;
        }

        trim(buf);
        if (!buf[0]) continue;

        if (!strcmp(buf, "/exit") || !strcmp(buf, "/quit")) {
            printf("%s\n", ui_bye);
            break;
        }

        if (!strcmp(buf, "/model")) {
            printf("%s: %s\n", ui_model, model);
            continue;
        }

        if (!strcmp(buf, "/history")) {
            print_history();
            continue;
        }

        if (!strcmp(buf, "/clear")) {
            for (int i = 0; i < TURN_COUNT; i++) {
                free(TURNS[i].user);
                free(TURNS[i].assistant);
            }
            TURN_COUNT = 0;
            printf("%s%s%s\n", COLOR_INFO, ui_hist_cleared, COLOR_RESET);
            continue;
        }

        if (!strcmp(buf, "/ml") || !strcmp(buf, "/multi")) {
            char *multi = read_multiline_prompt();
            if (!multi) {
                continue;
            }
            log_msg("Modelden cevap bekleniyor (çok satırlı)...");
            char *answer = call_openai(api_key, model, multi);
            if (answer) {
                printf("\n%sChatGPT:%s\n%s\n", COLOR_ASSIST, COLOR_RESET, answer);
                extract_commands_from_answer(answer);
                if (LAST_CMD_COUNT > 0) {
                    printf("%s%s:%s\n", COLOR_CMD, ui_suggested, COLOR_RESET);
                    for (int i = 0; i < LAST_CMD_COUNT; i++) {
                        printf("  [%d] $ %s\n", i + 1, LAST_CMDS[i]);
                    }
                    printf("%s\n", ui_run_hint);
                }
                add_turn(multi, answer);
                free(answer);
            }
            free(multi);
            continue;
        }

        if (!strncmp(buf, "/run", 4)) {
            if (LAST_CMD_COUNT == 0) {
                printf("%s%s%s\n", COLOR_INFO, ui_no_cmd, COLOR_RESET);
                continue;
            }
            int idx = 0;
            if (sscanf(buf + 4, "%d", &idx) != 1 || idx < 1 || idx > LAST_CMD_COUNT) {
                printf("%s%s (1-%d).%s\n", COLOR_ERROR, ui_invalid_num, LAST_CMD_COUNT, COLOR_RESET);
                continue;
            }
            const char *cmd = LAST_CMDS[idx - 1];
            printf("%s%s:%s %s\n", COLOR_CMD, ui_run_cmd, COLOR_RESET, cmd);
            printf("%s", ui_confirm);
            char ans[16];
            if (!fgets(ans, sizeof(ans), stdin)) {
                continue;
            }
            if (ans[0] == 'y' || ans[0] == 'Y') {
                log_msg("Shell komutu system() ile çalıştırılıyor...");
                int rc = system(cmd);
                printf("%s%s:%s %d\n", COLOR_INFO, ui_ret_code, COLOR_RESET, rc);
            } else {
                printf("%s\n", ui_cancelled);
            }
            continue;
        }

        log_msg("Modelden cevap bekleniyor...");
        char *answer = call_openai(api_key, model, buf);
        if (answer) {
            printf("\n%sChatGPT:%s\n%s\n", COLOR_ASSIST, COLOR_RESET, answer);
            extract_commands_from_answer(answer);
            if (LAST_CMD_COUNT > 0) {
                printf("%s%s:%s\n", COLOR_CMD, ui_suggested, COLOR_RESET);
                for (int i = 0; i < LAST_CMD_COUNT; i++) {
                    printf("  [%d] $ %s\n", i + 1, LAST_CMDS[i]);
                }
                printf("%s\n", ui_run_hint);
            }
            add_turn(buf, answer);
            free(answer);
        } else {
            log_msg("Cevap alınamadı (boş veya hata).");
        }
    }

    free(api_key);
    free(model_cfg);
    free(buf);
    clear_last_cmds();
    for (int i = 0; i < TURN_COUNT; i++) {
        free(TURNS[i].user);
        free(TURNS[i].assistant);
    }

    return 0;
}
