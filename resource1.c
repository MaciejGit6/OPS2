#define RING_SIZE 32

typedef struct {
    data_t buf[RING_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mtx;
    sem_t sem;
} ring_buffer_t;

void init_ring(ring_buffer_t* rb) {
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    pthread_mutex_init(&rb->mtx, NULL);
    sem_init(&rb->sem, 0, 0);
}

int push_ring(ring_buffer_t* rb, data_t d) {
    int success = 0;
    pthread_mutex_lock(&rb->mtx);
    if (rb->count < RING_SIZE) {
        rb->buf[rb->tail] = d;
        rb->tail = (rb->tail + 1) % RING_SIZE;
        rb->count++;
        success = 1;
    }
    pthread_mutex_unlock(&rb->mtx);
    if (success) {
        sem_post(&rb->sem);
    } else {
        fprintf(stderr, "Ring buffer full\n");
    }
    return success;
}

data_t pop_ring(ring_buffer_t* rb) {
    sem_wait(&rb->sem);
    pthread_mutex_lock(&rb->mtx);
    data_t d = rb->buf[rb->head];
    rb->head = (rb->head + 1) % RING_SIZE;
    rb->count--;
    pthread_mutex_unlock(&rb->mtx);
    return d;
}

//-----------------

typedef struct node_t {
    data_t data;
    struct node_t* next;
} node_t;

//-----------------

typedef struct {
    node_t* head;
    node_t* tail;
    pthread_mutex_t mtx;
    sem_t sem;
} ll_queue_t;

void init_llq(ll_queue_t* q) {
    q->head = NULL;
    q->tail = NULL;
    pthread_mutex_init(&q->mtx, NULL);
    sem_init(&q->sem, 0, 0);
}

void push_llq(ll_queue_t* q, data_t d) {
    node_t* new_node = (node_t*)malloc(sizeof(node_t));
    if (!new_node) return;
    new_node->data = d;
    new_node->next = NULL;
    
    pthread_mutex_lock(&q->mtx);
    if (q->tail == NULL) {
        q->head = new_node;
        q->tail = new_node;
    } else {
        q->tail->next = new_node;
        q->tail = new_node;
    }
    pthread_mutex_unlock(&q->mtx);
    
    sem_post(&q->sem);
}

data_t pop_llq(ll_queue_t* q) {
    sem_wait(&q->sem);
    
    pthread_mutex_lock(&q->mtx);
    node_t* old_head = q->head;
    data_t d = old_head->data;
    
    q->head = old_head->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    pthread_mutex_unlock(&q->mtx);
    
    free(old_head);
    return d;
}

//-----------------

typedef struct {
    node_t* head;
    pthread_mutex_t mtx;
    sem_t sem;
} ll_stack_t;

void init_lls(ll_stack_t* s) {
    s->head = NULL;
    pthread_mutex_init(&s->mtx, NULL);
    sem_init(&s->sem, 0, 0);
}

void push_lls(ll_stack_t* s, data_t d) {
    node_t* new_node = (node_t*)malloc(sizeof(node_t));
    if (!new_node) return;
    new_node->data = d;
    
    pthread_mutex_lock(&s->mtx);
    new_node->next = s->head;
    s->head = new_node;
    pthread_mutex_unlock(&s->mtx);
    
    sem_post(&s->sem);
}

data_t pop_lls(ll_stack_t* s) {
    sem_wait(&s->sem);
    
    pthread_mutex_lock(&s->mtx);
    node_t* old_head = s->head;
    data_t d = old_head->data;
    
    s->head = old_head->next;
    pthread_mutex_unlock(&s->mtx);
    
    free(old_head);
    return d;
}

//-----------------

#define HEAP_SIZE 64

typedef struct {
    data_t buf[HEAP_SIZE];
    int size;
    pthread_mutex_t mtx;
    sem_t sem;
} minheap_t;

void init_heap(minheap_t* h) {
    h->size = 0;
    pthread_mutex_init(&h->mtx, NULL);
    sem_init(&h->sem, 0, 0);
}

void swap_data(data_t* a, data_t* b) {
    data_t temp = *a;
    *a = *b;
    *b = temp;
}

int push_heap(minheap_t* h, data_t d) {
    int success = 0;
    pthread_mutex_lock(&h->mtx);
    if (h->size < HEAP_SIZE) {
        int i = h->size++;
        h->buf[i] = d;
        while (i != 0 && h->buf[(i - 1) / 2].P > h->buf[i].P) {
            swap_data(&h->buf[i], &h->buf[(i - 1) / 2]);
            i = (i - 1) / 2;
        }
        success = 1;
    }
    pthread_mutex_unlock(&h->mtx);
    if (success) sem_post(&h->sem);
    return success;
}

data_t pop_heap(minheap_t* h) {
    sem_wait(&h->sem);
    pthread_mutex_lock(&h->mtx);
    data_t root = h->buf[0];
    h->buf[0] = h->buf[--h->size];
    int i = 0;
    while (1) {
        int left = 2 * i + 1;
        int right = 2 * i + 2;
        int smallest = i;
        if (left < h->size && h->buf[left].P < h->buf[smallest].P)
            smallest = left;
        if (right < h->size && h->buf[right].P < h->buf[smallest].P)
            smallest = right;
        if (smallest != i) {
            swap_data(&h->buf[i], &h->buf[smallest]);
            i = smallest;
        } else {
            break;
        }
    }
    pthread_mutex_unlock(&h->mtx);
    return root;
}

//-----------------

typedef struct bst_node {
    data_t data;
    struct bst_node* left;
    struct bst_node* right;
} bst_node_t;

typedef struct {
    bst_node_t* root;
    pthread_mutex_t mtx;
} bst_t;

void init_bst(bst_t* tree) {
    tree->root = NULL;
    pthread_mutex_init(&tree->mtx, NULL);
}

bst_node_t* insert_node(bst_node_t* node, data_t d) {
    if (node == NULL) {
        bst_node_t* new_node = (bst_node_t*)malloc(sizeof(bst_node_t));
        new_node->data = d;
        new_node->left = NULL;
        new_node->right = NULL;
        return new_node;
    }
    int cmp = strcmp(d.divis_name, node->data.divis_name);
    if (cmp < 0) {
        node->left = insert_node(node->left, d);
    } else if (cmp > 0) {
        node->right = insert_node(node->right, d);
    } else {
        node->data.X = d.X;
        node->data.Y = d.Y;
    }
    return node;
}

void bst_insert(bst_t* tree, data_t d) {
    pthread_mutex_lock(&tree->mtx);
    tree->root = insert_node(tree->root, d);
    pthread_mutex_unlock(&tree->mtx);
}

int search_node(bst_node_t* node, const char* name, data_t* out_data) {
    if (node == NULL) return 0;
    int cmp = strcmp(name, node->data.divis_name);
    if (cmp == 0) {
        *out_data = node->data;
        return 1;
    } else if (cmp < 0) {
        return search_node(node->left, name, out_data);
    } else {
        return search_node(node->right, name, out_data);
    }
}

int bst_search(bst_t* tree, const char* name, data_t* out_data) {
    pthread_mutex_lock(&tree->mtx);
    int found = search_node(tree->root, name, out_data);
    pthread_mutex_unlock(&tree->mtx);
    return found;
}