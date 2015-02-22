#include <curl/curl.h>
#include <assert.h>
#include <libxml/SAX2.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libnotify/notify.h>

#define CONFIG_FILE ".rssrc"
#define LEN_SITE_NAME 32
#define LEN_URL 128
#define LEN_ETAG 128
#define LEN_MAX 128

typedef struct
{
  pthread_t thread_id;
  unsigned i;
  time_t last_pubDate;  //retrieve from last fetch
  char site_name[LEN_SITE_NAME];
  char etag[LEN_ETAG];
  char url[LEN_URL];
} rss_thread;

struct MemStruct
{
  char *mem;
  size_t size;
};

typedef struct
{
  char state;   // 1:title 2:pubDate
  char is_ok;
  char in_item;
  unsigned int depth;
  struct MemStruct content;
  time_t last_pubDate;
  rss_thread* rssthread;
  NotifyNotification* notification;
} xml_node;

static void rss_on_startelem(void   *ctx, const xmlChar*name, const xmlChar**atts);
static void rss_on_endelem(void     *ctx, const xmlChar*name);
static void rss_on_characters(void  *ctx, const xmlChar*ch, int len);
static void rss_on_enddoc(void      *ctx);

static void write_config(xml_node   *);
static int  read_config(const       char *);

//global
char path[32];
pthread_mutex_t config_mutex;

size_t
parse_rss_callback(char* in, size_t size, size_t nmemb, void* storage)
{
  xmlParserCtxt* ctxt = storage;
  xml_node* curr_node = ctxt->userData;
  if(curr_node->is_ok == 0)
    return(0);
  if(xmlParseChunk(ctxt, in, size*nmemb, 0) == 0)
      return(size*nmemb);
  xmlParserError(ctxt, "xmlParseChunk");
  return(0);
}

void*
fetch(void* rssthread_ptr)
{
  //Curl for rss fetch
  CURL *curl = curl_easy_init();
  CURLcode res;

  xml_node current;
  memset(&current, 0, sizeof(xml_node));
  current.is_ok = 1;

  rss_thread* rssthread = rssthread_ptr;
  printf("----------------------------thread %d start!----------------------------\n", rssthread->i);
  current.last_pubDate = rssthread->last_pubDate;
  current.rssthread = rssthread;
  notify_init(rssthread->site_name);

  xmlSAXHandler sax_handler;
  memset(&sax_handler, 0, sizeof(xmlSAXHandler));
  sax_handler.initialized = XML_SAX2_MAGIC;
  sax_handler.startElement= rss_on_startelem;
  sax_handler.endElement= rss_on_endelem;
  sax_handler.characters = rss_on_characters;
  sax_handler.endDocument = rss_on_enddoc;

  xmlParserCtxt* parse_context = xmlCreatePushParserCtxt(&sax_handler, &current, 0, 0, NULL);

  assert(curl != NULL);
  curl_easy_setopt(curl, CURLOPT_URL, rssthread->url);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 40960);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef CURL_DEBUG
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
#endif
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, parse_rss_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, parse_context);

  printf("fetching from %s(%s):\n", rssthread->site_name, rssthread->url);
  if((res = curl_easy_perform(curl)) != CURLE_OK)
    pthread_exit(NULL);
  else
    xmlParseChunk(parse_context, NULL, 0, 1);

  free(current.content.mem);
  curl_easy_cleanup(curl);
  pthread_exit(NULL);
}

static void
rss_on_startelem(void* ctx, const xmlChar* name, const xmlChar** attr)
{
  xml_node* curr_node = ctx;
  curr_node->depth++;
  if(!xmlStrcmp(name, "item") )
    curr_node->in_item = 1;
  if(curr_node->depth != 4 || curr_node->in_item != 1)
    return;
  if(!xmlStrcmp(name, "title") )
  {
    curr_node->state = 1;//title mode
    //clean up
    free(curr_node->content.mem);
    curr_node->content.mem = NULL;
    curr_node->content.size = 0;
  }
  else if(!xmlStrcmp(name, "pubDate"))
    curr_node->state = 2;//pubdate mode
}

static void
rss_on_endelem(void* ctx, const xmlChar* name)
{
  xml_node* curr_node = ctx;
  curr_node->depth--;
  char* mem_addr = curr_node->content.mem; //pass value
  if(!xmlStrcmp(name, "item") )
    curr_node->in_item = 1;

  if(curr_node->state == 2 && curr_node->is_ok == 1)
  {
#ifdef DEBUG
    printf("%s, %d\n", mem_addr, curr_node->is_ok);
#endif
    curr_node->notification = notify_notification_new(curr_node->rssthread->site_name, NULL, NULL);
    notify_notification_update(curr_node->notification, curr_node->rssthread->site_name, mem_addr,  NULL);
    notify_notification_show(curr_node->notification, NULL);
  }
  curr_node->state = 0;
}

static void
rss_on_characters(void* ctx, const xmlChar* ch, int len)
{
  xml_node* curr_node = ctx;
  char** mem_addr = &curr_node->content.mem; //pass address
  size_t* mem_size = &curr_node->content.size;//pass address
  time_t pubDate;
  char time[32];

  switch(curr_node->state)
  {
    case 1:
    if((*mem_addr = realloc(*mem_addr, sizeof(char)*(*mem_size + len + 1))) == NULL)
    {
      printf("OOM.\n");
      curr_node->is_ok = 0;
      return;
    }
    memcpy(*mem_addr+sizeof(char)*(*mem_size), ch, sizeof(char)*len);
    *mem_size += len;
    (*mem_addr)[*mem_size] = 0;
    break;
    case 2:
    strncpy(time, ch, len);
    time[len] = 0;
    pubDate = curl_getdate(time, NULL);
    if(pubDate > curr_node->last_pubDate)
      curr_node->last_pubDate = pubDate;
    //printf("\n");
    if(pubDate <= curr_node->rssthread->last_pubDate) //display only new feeds
    {
      curr_node->is_ok = 0;
#ifdef DEBUG
      printf("\n----------------------------thread %d: so much for new feeds.----------------------------\n", curr_node->rssthread->i);
#endif
      write_config(curr_node);
      pthread_exit(NULL);
    }
#ifdef DEBUG
    printf("pubDate[%d]: %s ::: %d :: %d :: %d--->",curr_node->rssthread->i, time, pubDate, curr_node->last_pubDate, curr_node->rssthread->last_pubDate);
#endif
    break;
    default:
    return;
  }
}

static void
rss_on_enddoc(void* ctx)
{
  xml_node* curr_node = ctx;
  write_config(curr_node);
  printf("Config file written\n");
  pthread_exit(NULL);
}

static int
read_config(const char* filename)
{
  unsigned int i;
  rss_thread* rssthread = NULL;
  xmlDoc* doc =  xmlParseFile(filename);
  assert(doc != NULL);
  xmlXPathContext* xpath_ctx = xmlXPathNewContext(doc);
  assert(xpath_ctx != NULL);
  xmlXPathObject*  xpath_obj = xmlXPathEvalExpression("/sites/*", xpath_ctx); //Get all sites
  xmlXPathObject* xpath_content;
  assert(xpath_obj != NULL);

  xmlNodeSet* node_set = xpath_obj->nodesetval;
  char* nodes;
  rssthread = calloc(node_set->nodeNr, sizeof(rss_thread));
#ifdef DEBUG
  printf("%d thread(s) in total.\n", node_set->nodeNr);
#endif
  for(i = 0; i < node_set->nodeNr; i++)
  {
    (rssthread+i)->i = i;
    strncpy((rssthread+i)->site_name, node_set->nodeTab[i]->name, LEN_SITE_NAME);
    xpath_content = xmlXPathEvalExpression("//url", xpath_ctx);
    assert(xpath_content != NULL);
    nodes = xpath_content->nodesetval->nodeTab[i]->children->content;
#ifdef DEBUG
#endif
    strncpy((rssthread+i)->url, nodes, LEN_URL);

    xpath_content = xmlXPathEvalExpression("//last_pubDate", xpath_ctx);
    assert(xpath_content != NULL);
    nodes = xpath_content->nodesetval->nodeTab[i]->children->content;
    (rssthread+i)->last_pubDate = atoi(nodes);
    pthread_create(&(rssthread+i)->thread_id, NULL, fetch, (void*)(rssthread+i));
  }
  xmlFreeDoc(doc);
  printf("doc freed.\n");
  for(i = 0; i < node_set->nodeNr; i++)
  {
    pthread_join((rssthread+i)->thread_id, NULL);
#ifdef DEBUG
    printf("thread %d destroyed.\n", (rssthread+i)->i);
#endif
  }
  xmlXPathFreeObject(xpath_obj);
  xmlXPathFreeContext(xpath_ctx);

  free(rssthread);
#ifdef DEBUG
  printf("all thread(s) destroyed.\n");
#endif
  return(0);
}

static void
write_config(xml_node* xmlnode)
{
  if(xmlnode->last_pubDate <= xmlnode->rssthread->last_pubDate)
    goto UNCHANGED;
  printf("thread %d:trying to write config file \n", xmlnode->rssthread->i);
  pthread_mutex_lock(&config_mutex);
  /*
  strncpy(exp, xmlnode->rssthread->site_name, LEN_SITE_NAME);
  char exp[LEN_SITE_NAME+7] = "/sites/\0";
  */
  char exp[LEN_SITE_NAME+7];
  xmlDoc* doc =  xmlParseFile(path);
  assert(doc != NULL);
  xmlXPathContext* xpath_ctx = xmlXPathNewContext(doc);
  assert(xpath_ctx != NULL);
  xmlXPathObject* xpath_content = xmlXPathEvalExpression("//last_pubDate", xpath_ctx);
  assert(xpath_content != NULL);
  xmlNode* nodes = xpath_content->nodesetval->nodeTab[xmlnode->rssthread->i];
  sprintf(exp, "%d", xmlnode->last_pubDate);
  xmlNodeSetContent(nodes, exp);
  xmlSaveFile(path, doc);
  xmlFreeDoc(doc);
  printf("doc freed.\n");
  xmlXPathFreeObject(xpath_content);
  xmlXPathFreeContext(xpath_ctx);
  pthread_mutex_unlock(&config_mutex);
  printf("thread %d:config file written.\n", xmlnode->rssthread->i);
  return;
UNCHANGED:
  printf("thread %d:no change made, exit.\n", xmlnode->rssthread->i);
}

int
main()
{
  sprintf(path, "%s/%s", getenv("HOME"), CONFIG_FILE);
  xmlInitParser();
  pthread_mutex_init(&config_mutex, NULL);
  curl_global_init(CURL_GLOBAL_ALL);
  while(1)
  {
    assert(read_config(path) == 0);
    sleep(300);
  }
  xmlCleanupParser();
  xmlMemoryDump();
  pthread_mutex_destroy(&config_mutex);
  return(EXIT_SUCCESS);
}

