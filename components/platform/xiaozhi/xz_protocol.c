#include "xz_protocol.h"
#include <string.h>
const char *xz_display_text(const char *text) {
  if (!text) return "";
  if (strstr(text, "self.weather.") || strstr(text, "get_forecast") ||
      strstr(text, "weather.get_forecast")) return "正在查询天气";
  if (strstr(text, "self.radio.") || strstr(text, "list_stations") ||
      strstr(text, "play_station")) return "正在查找电台";
  // A few providers expose the call's arguments as a separate subtitle.
  const char *p = text;
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  if (*p == '{' || *p == '[' || !strncmp(p, "```json", 7) ||
      !strncmp(p, "<tool_call", 10)) {
    if (strstr(p, "\"city\"") && !strstr(p, "\"station\"")) return "正在查询天气";
    if (strstr(p, "\"station\"") || strstr(p, "\"query\"")) return "正在查找电台";
    if (strstr(p, "\"tool_calls\"") || strstr(p, "\"arguments\"")) return "正在查询";
  }
  return text;
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
bool xz_hex16(const char *s, uint8_t out[16]) {
  if (!s || strlen(s) != 32)
    return false;
  uint8_t decoded[16];
  for (unsigned i = 0; i < 16; i++) {
    int a = hex(s[2 * i]), b = hex(s[2 * i + 1]);
    if (a < 0 || b < 0)
      return false;
    decoded[i] = (a << 4) | b;
  }
  memcpy(out, decoded, 16);
  return true;
}
bool xz_udp_valid(const uint8_t *p, size_t n, uint32_t prev, uint32_t *seq) {
  if (!p || n < 17 || n > XZ_PACKET_MAX + 16 || p[0] != 1)
    return false;
  if ((((unsigned)p[2] << 8) | p[3]) != n - 16)
    return false;
  uint32_t s = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 |
               (uint32_t)p[14] << 8 | p[15];
  if (s <= prev)
    return false;
  if (seq)
    *seq = s;
  return true;
}
#include "xz_text_pairs.inc"
static unsigned scalar(const unsigned char *p, unsigned *length) {
  unsigned c=*p;*length=1;
  if(c<128)return c;
  unsigned n=c>=0xf0&&c<=0xf4?4:c>=0xe0&&c<=0xef?3:c>=0xc2&&c<=0xdf?2:0;
  if(!n)return 0xfffd;
  unsigned v=c & (127u>>n);
  for(unsigned i=1;i<n;i++){if((p[i]&0xc0)!=0x80)return 0xfffd;v=v*64+(p[i]&63);}
  if((n==3&&v<0x800)||(n==4&&v<0x10000)||(v>=0xd800&&v<=0xdfff)||v>0x10ffff)return 0xfffd;
  *length=n;return v;
}
static unsigned compose(unsigned a,unsigned b) {
  if(a>=0x1100&&a<=0x1112&&b>=0x1161&&b<=0x1175)
    return 0xac00+((a-0x1100)*21+b-0x1161)*28;
  if(a>=0xac00&&a<=0xd7a3&&!((a-0xac00)%28)&&b>=0x11a8&&b<=0x11c2)
    return a+b-0x11a7;
  if(a>65535||b>65535)return 0;
  uint32_t key=a<<16|b;
  unsigned lo=0,hi=sizeof text_pairs/sizeof text_pairs[0];
  while(lo<hi){unsigned m=(lo+hi)/2;uint32_t k=(uint32_t)text_pairs[m][0]<<16|text_pairs[m][1];if(k<key)lo=m+1;else hi=m;}
  return lo<sizeof text_pairs/sizeof text_pairs[0]&&text_pairs[lo][0]==a&&text_pairs[lo][1]==b?text_pairs[lo][2]:0;
}
void xz_text_copy(char *out,size_t cap,const char *text) {
  if(!out||!cap)return;
  const unsigned char *p=(const unsigned char *)(text?text:"");
  size_t used=0;
  while(*p){
    unsigned n,c=scalar(p,&n);p+=n;
    while(*p){unsigned step,second=scalar(p,&step),joined=compose(c,second);if(!joined)break;c=joined;p+=step;}
    n=c<128?1:c<0x800?2:c<0x10000?3:4;
    if(n>=cap-used)break;
    unsigned v=c;
    for(unsigned i=n-1;i;i--){out[used+i]=(char)(128+(v&63));v>>=6;}
    out[used]=(char)(n==1?v:(n==2?0xc0:n==3?0xe0:0xf0)|v);used+=n;
  }
  out[used]=0;
}
bool xz_fragment(xz_json_buffer_t *b, const char *p, int n, int offset,
                 int total) {
  if (offset == 0)
    b->used = b->total = 0;
  if (!p || n <= 0 || offset < 0 || total <= 0 || total > XZ_JSON_MAX ||
      offset > total || n > total - offset || (size_t)offset != b->used ||
      (offset && (size_t)total != b->total)) {
    b->used = b->total = 0;
    return false;
  }
  b->total = total;
  memcpy(b->data + offset, p, n);
  b->used += n;
  b->data[b->used] = 0;
  return b->used == b->total;
}

// ASR strings are short-lived. Recognize a complete closing speech act and
// reject quoted, negated or explanatory mentions of the same words.
static size_t matching_prefix(const char *text, const char *const *words, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    size_t n = strlen(words[i]);
    if (strncmp(text, words[i], n) == 0) return n;
  }
  return 0;
}
#define COUNT_WORDS(a) (sizeof(a) / sizeof((a)[0]))
static bool closing_clause(const char *text) {
  static const char *const lead[] = {"谢谢你", "谢谢", "好吧", "好的", "好啦", "好了", "好", "小智", "嗯嗯", "嗯", "那就", "那么", "那", "今天", "咱们", "我们", "就"};
  static const char *const acts[] = {"先聊到这里", "先聊到这儿", "聊到这里", "聊到这儿", "聊到这", "先到这里", "先到这儿", "到此为止", "先这样", "这样吧", "就这样", "我不想聊了", "不想聊了", "我先不聊了", "我先去忙了", "我该走了", "我得走了", "我先走了", "我先走啦", "我先下了", "先不聊了", "不聊了", "结束对话", "结束聊天", "下次再见", "下次再聊", "下次聊", "下次见", "明天见", "回头再聊", "回头聊", "回头见", "再会", "再见", "拜拜", "掰掰", "byebye", "bye"};
  static const char *const tail[] = {"小智", "谢谢你", "谢谢", "好啦", "啦", "了", "吧", "哦", "啊", "哈", "喽", "咯"};
  size_t n;
  while ((n = matching_prefix(text, lead, COUNT_WORDS(lead))) != 0) text += n;
  n = matching_prefix(text, acts, COUNT_WORDS(acts));
  if (!n) return false;
  text += n;
  while (*text) {
    n = matching_prefix(text, tail, COUNT_WORDS(tail));
    if (!n) n = matching_prefix(text, acts, COUNT_WORDS(acts));
    if (!n) return false;
    text += n;
  }
  return true;
}

bool xz_is_farewell(const char *text) {
  if (!text) return false;
  static const char *const spaces[] = {" ", "\t", "\r", "\n", "　", "-"};
  static const char *const punctuation[] = {"，", "。", "！", "、", "；", "～", "…", ",", ".", "!", ";", "~"};
  static const char *const mentions[] = {"？", "?", "“", "”", "「", "」", "\"", "怎么", "什么意", "翻译", "为什么", "解释", "读法", "教我", "歌词", "故事", "举例", "比如", "如果", "假如", "听你说", "跟我说", "请你说", "请说", "他说", "她说", "别说", "别跟", "不要", "不想", "不说", "不是", "别结束", "不结束"};
  char compact[256];
  size_t used = 0, clause = 0;
  bool boundary = false;
  while (*text) {
    size_t n = matching_prefix(text, spaces, COUNT_WORDS(spaces));
    if (n) { text += n; continue; }
    n = matching_prefix(text, punctuation, COUNT_WORDS(punctuation));
    if (n) { text += n; boundary = true; continue; }
    if (used + 1 >= sizeof compact) return false;
    if (boundary) { clause = used; boundary = false; }
    unsigned char c = (unsigned char)*text++;
    compact[used++] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c;
  }
  compact[used] = 0;
  if (closing_clause(compact)) return true;
  if (!clause) return false;
  for (size_t i = 0; i < COUNT_WORDS(mentions); ++i)
    if (strstr(compact, mentions[i])) return false;
  return closing_clause(compact + clause);
}
