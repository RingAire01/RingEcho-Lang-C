#include "backend/backend_c_internal.h"
#include <stdio.h>
#include <stdlib.h>

void *__real_realloc(void *,size_t);
void *__real_calloc(size_t,size_t);
char *__real_strdup(const char *);
static unsigned calls,fail_at;
static bool failing(void){return ++calls==fail_at;}
void *__wrap_realloc(void *p,size_t n){return failing()?NULL:__real_realloc(p,n);}
void *__wrap_calloc(size_t n,size_t size){return failing()?NULL:__real_calloc(n,size);}
char *__wrap_strdup(const char *s){return failing()?NULL:__real_strdup(s);}

static bool exercise(unsigned failure,unsigned *total) {
    Re0ErrorList errors;re0_error_list_init(&errors);
    Re0SemanticModel model;re0_model_init(&model);
    Re0StmtVec declarations;Re0StmtVec_init(&declarations);
    Re0Codegen codegen;re0_codegen_init(&codegen,&errors,&model,&re0_backend_c);
    re0_buffer_write_str(&codegen.output,"/* original */");
    reset_c_state();
    Re0Type byte={.kind=RE0_TYPE_U8};
    Re0Type array={.kind=RE0_TYPE_ARRAY,.array={.inner=&byte,.size=3}};
    Re0Type reference={.kind=RE0_TYPE_REFERENCE,.ref_={.inner=&array,.mutable_=false}};
    Re0Type *args[]={&array,&reference};
    Re0Type function={.kind=RE0_TYPE_FN,.func={.ret=&reference,.params=args,.param_count=2}};
    calls=0;fail_at=failure;
    c_storage_begin(&codegen,&declarations);
    (void)c_storage_type(&function);
    c_storage_finish(&codegen);
    unsigned used=calls;fail_at=0;
    bool expected_success=!failure || failure>used;
    bool ok=expected_success ? !codegen.had_error && !re0_buffer_failed(&codegen.output) : codegen.had_error;
    if(!expected_success)ok=ok && codegen.output.len==sizeof("/* original */")-1;
    if(total)*total=used;
    re0_codegen_destroy(&codegen);re0_model_free(&model);re0_error_list_free(&errors);
    return ok;
}

int main(void) {
    unsigned total;
    if(!exercise(0,&total))return 1;
    for(unsigned i=1;i<=total;i++)if(!exercise(i,NULL)){fprintf(stderr,"C storage allocation failure %u was not propagated\n",i);return 1;}
    printf("C storage allocation faults passed (%u points)\n",total);
    return 0;
}
