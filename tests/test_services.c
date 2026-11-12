/* System services contracts: dialogs (progress bars run until closed), PlayGo, trophies. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_services.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int theordergpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int theordergpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 0; }
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
static int choice_next=-1;
int theordergpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) { (void)title; (void)items; (void)images; (void)focus; return count>0; }
int theordergpu_choice_poll(void) { return choice_next; }
static int trophy_notified=-1;
void theordergpu_trophy_unlocked(int id, const char *name, const char *detail, const unsigned char *png, uint64_t size) {
    (void)name; (void)detail; (void)png; (void)size; trophy_notified=id;
}
const char *runtime_file_user_dir(void) { return NULL; }

int runtime_savedata_param(int32_t user, const char *dir, void *param) { (void)user; (void)dir; (void)param; return -1; }
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) { (void)user; (void)dir; (void)out; (void)size; return -1; }

int main(void) {
    unsigned char param[128]={0}, result[64]={0}; /* the game zeroes the pointers it does not want */
    uint32_t value;

    /* MsgDialog: a user message finishes with OK; a progress bar runs until closed. */
    assert(msg_init()==0);
    memset(param+56,0,4); param[56]=1;
    assert(msg_open(param)==0 && msg_status()==DIALOG_FINISHED);
    assert(msg_result(result)==0);
    memcpy(&value,result+4,4); assert(value==1);
    param[56]=2;
    assert(msg_open(param)==0 && msg_status()==DIALOG_RUNNING);
    assert(msg_result(result)==DIALOG_NOT_FINISHED);
    assert(msg_progress(0,50)==0 && msg_progress(1,50)==DIALOG_PARAM_INVALID);
    assert(msg_close()==0 && msg_status()==DIALOG_FINISHED && msg_close()==DIALOG_NOT_RUNNING);
    assert(msg_progress(0,100)==DIALOG_NOT_RUNNING);
    assert(msg_result(NULL)==DIALOG_ARG_NULL);
    assert(msg_term()==0 && msg_open(NULL)==DIALOG_ARG_NULL);

    /* SaveDataDialog: mode at offset 52 (5 = progress bar); result after the mode field. */
    assert(save_init()==0 && save_ready()==1);
    param[52]=5;
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING && save_progress(0,10)==0);
    assert(save_close()==0 && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==0); /* closed by the game: no button */
    param[52]=2;
    assert(save_open(param)==0 && save_status()==DIALOG_FINISHED && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==1);
    assert(save_term()==0);

    /* Save list: the player's choice comes back as the save directory. */
    char names[2][32]={"SAVE0001","SAVE0002"}, chosen[32];
    unsigned char items[64]={0}, list[128]={0}, out[64]={0};
    const char *names_ptr=names[0]; uint32_t name_count=2; const char *new_title="New";
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&name_count,4); memcpy(items+32,&new_title,8);
    unsigned char *items_ptr=items; char *chosen_ptr=chosen;
    list[52]=1; list[56]=2; memcpy(list+72,&items_ptr,8);
    memcpy(out+16,&chosen_ptr,8);
    assert(save_init()==0 && save_open(list)==0 && save_status()==DIALOG_RUNNING);
    choice_next=2; /* entries: New save, SAVE0001, SAVE0002 */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));
    memcpy(&value,out+4,4); assert(value==0);
    memcpy(&value,out+8,4); assert(value==1);
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && chosen[0]==0); /* new save */
    assert(save_open(list)==0);
    choice_next=-2;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    assert(save_term()==0);

    /* PlayGo: installed, full speed, progress never divides by zero. */
    int32_t handle=0; uint64_t progress[2]; int32_t speed; uint16_t chunk=0;
    assert(playgo_init(NULL)==0 && playgo_open(&handle,NULL)==0);
    assert(playgo_get_speed(handle,&speed)==0 && speed==2);
    assert(playgo_progress(handle,&chunk,1,progress)==0 && progress[0]==progress[1] && progress[1]);
    assert(playgo_progress(handle,&chunk,0,progress)==PLAYGO_BAD_SIZE);
    assert(playgo_todo(handle,&chunk,1)==0 && playgo_todo(handle,NULL,1)==PLAYGO_BAD_POINTER);
    assert(playgo_close(handle)==0 && playgo_close(handle)==PLAYGO_BAD_HANDLE && playgo_terminate()==0);

    /* Trophies (SpartanPort): unlocks are saved in <user dir>/trophies.dat and reported back; a
     * second unlock of the same trophy answers 0 without a second notice. No trophy00.trp here:
     * no count, no platinum. The test's own folder keeps other files (trophy_names.txt) out. */
    char trophy_dir_path[512];
    const char *tmp=getenv("TEMP") ? getenv("TEMP") : "/tmp";
    snprintf(trophy_dir_path,sizeof(trophy_dir_path),"%s/theorder-services-test",tmp);
#ifdef _WIN32
    _mkdir(trophy_dir_path);
#else
    mkdir(trophy_dir_path,0700);
#endif
#ifdef _WIN32
    _putenv_s("THEORDER_USER_DIR",trophy_dir_path);
#else
    setenv("THEORDER_USER_DIR",trophy_dir_path,1);
#endif
    char trophy_file[600];
    snprintf(trophy_file,sizeof(trophy_file),"%s/trophies.dat",trophy_dir_path);
    remove(trophy_file);
    uint32_t flags[4]={~0u,~0u,~0u,~0u}, count=7;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && !flags[0] && !flags[3] && count==0);
    assert(trophy_unlock_state(1,1,NULL,&count)==TROPHY_INVALID && trophy_release(0)==TROPHY_INVALID);
    int32_t platinum=7;
    assert(trophy_unlock(1,1,5,&platinum)==0 && platinum==-1 && trophy_notified==5);
    trophy_notified=-1;
    assert(trophy_unlock(1,1,5,&platinum)==0 && trophy_notified==-1);
    assert(trophy_unlock(1,1,40,NULL)==0 && trophy_unlock(1,1,TROPHY_MAX,NULL)==TROPHY_INVALID_ID);
    assert(trophy_unlock(1,1,-1,NULL)==TROPHY_INVALID_ID);
    assert(trophy_unlock_state(1,1,flags,&count)==0 && flags[0]==(1u<<5) && flags[1]==(1u<<8));
    FILE *saved=fopen(trophy_file,"rb");
    uint32_t on_disk[4]={0};
    assert(saved && fread(on_disk,sizeof(on_disk),1,saved)==1 && on_disk[0]==(1u<<5) && on_disk[1]==(1u<<8));
    fclose(saved);
    char trophy_tmp[620];
    snprintf(trophy_tmp,sizeof(trophy_tmp),"%s.tmp",trophy_file);
    assert(fopen(trophy_tmp,"rb")==NULL); /* moved over trophies.dat, not left behind */
    /* The next start reads them back from the file. */
    memset(trophy_flags,0,sizeof(trophy_flags));
    trophy_ready=0;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && flags[0]==(1u<<5) && flags[1]==(1u<<8));
    remove(trophy_file);
#ifdef _WIN32
    _rmdir(trophy_dir_path);
#else
    rmdir(trophy_dir_path);
#endif

    float ratio=0; assert(safe_area(&ratio)==0 && ratio==1.0f && safe_area(NULL)==SYSTEM_PARAMETER);
    puts("services test: ok");
    return 0;
}
