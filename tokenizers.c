#include <string.h>

//--

int parse_tokens(char *buffer, const char *delim, char **tokens_out, int max_tokens) {
    if (buffer == NULL || tokens_out == NULL) return 0;
    
    int count = 0;
    char *saveptr; 
    
    char *token = strtok_r(buffer, delim, &saveptr);
    
    while (token != NULL && count < max_tokens) {
        
        tokens_out[count] = token; 
        count++; 
        
        token = strtok_r(NULL, delim, &saveptr);
    }
    
    return count;
}

//--

int parse_command_payload(char *buffer, const char *delim, char **command_out, char **payload_out) {
    if (buffer == NULL) return 0;
    
    char *saveptr;
    
    *command_out = strtok_r(buffer, delim, &saveptr);
    
    if (*command_out == NULL) {
        *payload_out = NULL;
        return 0;
    }
    
    if (saveptr != NULL && *saveptr != '\0') {
        *payload_out = saveptr;
    } else {
        *payload_out = NULL; 
    }
    
    return 1;
}

//--

typedef struct {
    char *key;
    char *value;
} kv_pair_t;

int parse_key_value(char *buffer, const char *pair_delim, const char *kv_delim, kv_pair_t *pairs_out, int max_pairs) {
    if (buffer == NULL) return 0;
    
    int count = 0;
    
    char *main_bookmark; 
    char *pair_bookmark;  
    
    char *current_pair = strtok_r(buffer, pair_delim, &main_bookmark);
    
    while (current_pair != NULL && count < max_pairs) {
        
        char *key   = strtok_r(current_pair, kv_delim, &pair_bookmark);
        char *value = strtok_r(NULL,         kv_delim, &pair_bookmark);
        
        if (key != NULL && value != NULL) {
            pairs_out[count].key = key;
            pairs_out[count].value = value;
            count++;
        }
        
        current_pair = strtok_r(NULL, pair_delim, &main_bookmark);
    }
    
    return count;
}