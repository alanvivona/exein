/* Copyright 2020 Exein. All Rights Reserved.

Licensed under the GNU General Public License, Version 3.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.gnu.org/licenses/gpl-3.0.html

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model.h>
#include <tensorflow/lite/optional_debug_tools.h>

#include <xtensor/xarray.hpp>
#include <xtensor/xio.hpp>
#include <xtensor/xadapt.hpp>

#include "INIReader.h"
extern "C" {
#include <libexnl.h>
#include <sys/types.h>
#include <unistd.h>
}

//#define DEBUG
#ifdef DEBUG
#define DODEBUG( ... ) printf( __VA_ARGS__ ); fflush(stdout);
#else
#define DODEBUG( ... ) do { } while(0)
#endif


using namespace tflite;

//#define EXEIN_DEBUG
#define QUEUESIZE 16

#define TFLITE_MINIMAL_CHECK(x)                                  \
    if (!(x)) {                                                  \
        fprintf(stderr, "Error at %s:%d\n", __FILE__, __LINE__); \
        exit(1);                                                 \
    }

#define LOG(x) std::cerr
uint16_t data[EXEIN_BUFFES_SIZE];
typedef struct {
    int16_t index;
    pid_t pids[QUEUESIZE];
} pidqueue;


pidqueue addpid, *terminate;
std::map<int, xt::xarray<float>> predictions;
std::map<int, xt::xarray<float>> errors;
std::map<int, xt::xarray<int>> old_hooks;
uint16_t pidl4 = 0;
pid_t tmppid, tmppid2;


static void stack_tr() {
    void *trace[EXEIN_BACKTRACE_SIZE];
    char **messages = (char **)NULL;
    int i, trace_size = 0;

    trace_size = backtrace(trace, EXEIN_BACKTRACE_SIZE);
    messages = backtrace_symbols(trace, trace_size);
    printf("[stack trace(%d) ]>>>\n", trace_size);
    for (i=0; i < trace_size; i++)
        printf("%s\n", messages[i]);
    printf("<<<[stack trace]\n");
    free(messages);
}


void sigsegv_handler(int sig, siginfo_t *si, void *unused) {
    switch (sig) {
        case SIGSEGV:
            printf("sigsegv_handler - pid %d got SIGSEGV at address: 0x%lx\n", getpid(), (long) si->si_addr);
            stack_tr();
            exit(-1);
        case SIGCHLD:
            DODEBUG("sigchld_handler - pid %d got SIGCHLD from %d\n", getpid(), si->si_pid);
            wait(NULL);
            break;
        default:
            printf("sigsegv_handler -[%d] Reecived Signal :%d\n",getpid(), sig);
    };
}


pid_t dequeue(pidqueue *q, int remove){
    if (q->index>-1) {
        DODEBUG("dequeue: index=%d return=%d\n", q->index, q->pids[q->index]);
        return q->pids[remove==0?q->index:q->index--];
    } else return 0;
}


void inqueue(pidqueue *q, pid_t p){
    DODEBUG("inqueue: index=%d val=%d\n", q->index, p);
    if (q->index<QUEUESIZE) q->pids[++q->index]=p;
}


// Utility functions

std::vector<int> split(std::string str) {
    std::stringstream ss(str);
    std::string s;
    std::vector<int> hooks;

    while (getline(ss, s, ',')) {
        hooks.push_back(std::stoi(s));
    }

    return hooks;
}


xt::xarray<int> isin(int hid, std::vector<int> item, int val, int notval) {
    xt::xarray<int>::shape_type sh0 = {1, item.size()};
    auto res = xt::empty<int>(sh0);
    res.fill(notval);
    for (size_t i = 0; i < item.size(); i++) {
        if (hid == item[i]) {
            res(i) = val;
            break;
        }
    }

  return res;
}


xt::xarray<int> isnotin(xt::xarray<int> arr, std::vector<int> item, int val) {
    xt::xarray<int>::shape_type sh0 = arr.shape();
    auto res = xt::empty<int>(sh0);
    res.fill(val);
    for (auto i: item) {
        xt::xarray<int>::iterator iter = arr.begin();
        while ((iter = std::find(iter, arr.end(), i)) != arr.end()) {
            int dis = std::distance(arr.begin(), iter);
            res(dis) = arr(dis);
            iter++;
        }
    }

    return res;
}



// TF player functions

xt::xarray<float> make_prediction(tflite::Interpreter* interpreter, xt::xarray<int> x, std::vector<int> output_shape) {
    // use tflite interpreter to perform inference
    auto input_array = xt::cast<float>(xt::expand_dims(x ,0));
    float* input = interpreter->typed_input_tensor<float>(0);

#ifdef EXEIN_DEBUG
    printf("Value stored in a variable input is: %f\n",*input);
#endif

    std::copy(input_array.begin(), input_array.end(), input);
    TFLITE_MINIMAL_CHECK(interpreter->Invoke() == kTfLiteOk);
    float* output_f = interpreter->typed_output_tensor<float>(0);

#ifdef EXEIN_DEBUG
    printf("Value stored in a variable output is: %f\n",*output_f);
#endif

    std::vector<float> output_v {output_f, output_f + output_shape[1]};
    xt::xarray<float> output = xt::adapt(output_v, output_shape);

    return output;
}


std::vector<int> get_tensor_shape(TfLiteTensor* tensor) {
    std::vector<int> shape;
    const int dims = tensor->dims->size;

    for (int i = 0; i < dims; ++i) {
        shape.push_back(tensor->dims->data[i]);
    }

    return shape;
}


xt::xarray<int> one_hot(xt::xarray<int> arr, std::vector<int> hooks, int ws) {
    // encode array of hooks as one-hot vector
    xt::xarray<int>::shape_type sh0 = {hooks.size(), arr.size()};
    auto res = xt::empty<int>(sh0);
    for (int h = 0; h < hooks.size(); h++) {
        for (int a = 0; a < arr.size(); a++) {
            res(a+h*ws) = (arr(a)==hooks[h]);
        }
    }

    return xt::transpose(res);
}


float cross_entropy(xt::xarray<float> predictions, xt::xarray<int> targets) {
    // compute cross entropy between latest prediction and actual observation (anomaly score)
    const double epsilon = std::pow(10, -12);

    predictions = xt::clip(predictions, epsilon, 1. - epsilon);
    auto xent = xt::sum(targets * xt::log(predictions), /*axis*/1);

    return -xent(0);
}


xt::xarray<float> update_error(xt::xarray<float> old_err_arr, float new_err) {
    // update prediction error array for current pid
    std::rotate(old_err_arr.begin(), old_err_arr.begin()+1, old_err_arr.end());
    old_err_arr(old_err_arr.size() - 1) = new_err;

    return old_err_arr;
}


/*
Load and initialize tflite interpreter and other tag-specific meta-parameters
:param models_dir: directory where .tflite and .ini saved files are stored
:param tag: tag we are interested in
:return: dictionary containing model and parameters for the tag
*/
std::map<string, string> initialize_exein(const char* config_file) {
    INIReader reader(config_file);
    std::map<string, string> model_params;

    model_params["window_size"] = reader.Get("PARAMS", "window_size", "15");
    model_params["rolling_size"] = reader.Get("PARAMS", "rolling_size", "60");
    model_params["tag"] = reader.Get("PARAMS", "tag", "-1");
    model_params["threshold"] = reader.Get("PARAMS", "threshold", "2.0");
    model_params["hooks"] = reader.Get("PARAMS", "hooks", "UNKNOWN");

    return model_params;
}


/*
Logic for the tag-specific MLE player.
:param hook_arr: array storing the last N hooks for the given PID
:param pid: pid that we want to investigate
:param tag_metaparms: dictionary containing all the necessary info for the tag (hooks, model, window_size, threshold and rolling_size)
:param: predictions: dictionary storing the last prediction for each PID (needed for checking signal)
:return:
*/
std::tuple<std::map<int, xt::xarray<float>>, std::map<int, xt::xarray<float>>, int>
mle_player(tflite::Interpreter* interpreter, xt::xarray<int> hook_arr, uint16_t pid,
           std::map<string, string> model_params, std::map<int, xt::xarray<float>> predictions,
           std::map<int, xt::xarray<float>> errors) {

    int index;
    xt::xarray<float> pred;
    DODEBUG("mle_player - begin \n");
    // get meta-parameters from dictionary
    std::vector<int> hooks = split(model_params["hooks"]);
    int window_size = std::stoi(model_params["window_size"]);
    float threshold = std::stof(model_params["threshold"]);
    int rolling_size = std::stoi(model_params["rolling_size"]);

    // turn hook sequence into feature for prediction
    index = rand()%( ((int)( hook_arr.end()-hook_arr.begin() )) - window_size - 1);

    DODEBUG("mle_player - index seed %d, windowsize=%d, array size=%d\n", index, window_size, hook_arr.end()-hook_arr.begin());
    std::vector<int> wsize_hook_arr_tmp(hook_arr.begin()+index, hook_arr.begin()+index+window_size);
    std::vector<std::size_t> shape = { 1, window_size };
    auto wsize_hook_arr = xt::adapt(wsize_hook_arr_tmp, shape);

#ifdef EXEIN_DEBUG
    std::cout << wsize_hook_arr << "\n";
#endif
    xt::xarray<int> x_tmp = isnotin(wsize_hook_arr, hooks, -1);
    xt::xarray<int> x = one_hot(x_tmp, hooks, window_size);

    int output = interpreter->outputs()[0];
    std::vector<int> output_shape = get_tensor_shape(interpreter->tensor(output));

    // make prediction and update prediction dict
    pred = make_prediction(interpreter, x, output_shape);

    predictions[pid] = pred;

    // check for signal
    int signal = 0;
    if (xt::mean(predictions[pid])() != 0) {
        xt::xarray<int> onehot_hid;
        int hid = hook_arr[index + window_size];

        // turn latest Hook ID into one-hot vecto
        if (std::find(hooks.begin(), hooks.end(), hid) != hooks.end())
            onehot_hid = isin(hid, hooks, 1, 0);
        else
            onehot_hid = isin(-1, hooks, 1, 0);

        // compute cross-ent between predicted and actually observed Hook ID
        float err = cross_entropy(predictions[pid], onehot_hid);
        if (xt::mean(errors[pid])() != 0) {
            errors[pid] = update_error(errors[pid], err);
        } else {
            xt::xarray<float> errors_p = xt::zeros<float>({1, rolling_size});
            errors[pid] = update_error(errors_p, err);
        }

        // compute moving average over the last rolling_size errors for the PID
        float m_avg_err = xt::mean(errors[pid])();
#ifdef EXEIN_DEBUG
        std::cout << "AVG: " << m_avg_err << "\n";
#endif

        // compare with threshold to check for an attack signal
        signal = (m_avg_err > threshold)? 1 : 0;
    }

    return std::make_tuple(predictions, errors, signal);
}


void new_pid_notify_cb(uint16_t pid) { //TODO: fork COW
    inqueue(&addpid, (pid_t) pid);
    printf("Now checking pid %d\n", pid);
}


void removed_pid_notify_cb(uint16_t pid) {
    inqueue(terminate, (pid_t) pid);
    printf("Removing pid %d\n", pid);
}


/*
Run the exein mle on historical data for the tag specified (simulate online execution).
:param data: dataset (numpy array) containing Hook IDs, PIDs and Tag
:param model_params: .tflite model and other meta parameters
:param tag: tag we are interested in
:return: Nothing, run the exein mle model
*/
int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "mle-player <secret> <config file> <tflite model> \n");
        return 1;
    }

    int				secret = std::stoi(argv[1]);
    const char			*config_file = argv[2];
    const char			*model_name = argv[3];
    std::map<string, string>	model_params;
    int				signal_ = 0;
    exein_shandle		*h;
    struct sigaction		sa = {0};


    srand(time(NULL));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) == -1) {
        printf("Receive feeds can't install the signal handler.");
    }

    terminate = (pidqueue *)  mmap(NULL, 512, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0); //probably it takes 4k, as a linux memory page is defined.
    terminate->index=-1;

    addpid.index=-1;
    model_params = initialize_exein(config_file);
    int tag = std::stoi(model_params["tag"]);

    std::cout << "Starting Exein monitoring for tag: " << tag << '\n';

    exein_new_pid_notify_cb = &new_pid_notify_cb;
    exein_delete_pid_cb = &removed_pid_notify_cb;

    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        printf("Receive feeds can't install the signal handler.");
    }

    if (!(h = exein_agent_start(secret, tag))) {
        std::cout << "Can't starting Exein agent" << '\n';
        return 1;
    }

    DODEBUG("model init\n");
    std::unique_ptr<tflite::FlatBufferModel> model = tflite::FlatBufferModel::BuildFromFile(model_name);
    TFLITE_MINIMAL_CHECK(model != nullptr);

    // Build the interpreter
    tflite::ops::builtin::BuiltinOpResolver resolver;
    InterpreterBuilder builder(*model, resolver);
    std::unique_ptr<Interpreter> interpreter;
    builder(&interpreter);
    TFLITE_MINIMAL_CHECK(interpreter != nullptr);
    DODEBUG("Model stuffs done\ninit tensors\n");
    // Allocate tensor buffers.
    TFLITE_MINIMAL_CHECK(interpreter->AllocateTensors() == kTfLiteOk);
    DODEBUG("Tensors done\n");
    while (true) { //pid specialized processes
        DODEBUG("MainLoop Itaration pid=%d\n", pidl4);
        if((pidl4!= 0)) {
            DODEBUG("[T%d-M%d] iteration started| ", pidl4, getpid());
            if (pidl4==dequeue(terminate,0)) {
                DODEBUG("[T%d-M%d] have been named. RIP| ", pidl4, getpid());
                exit(dequeue(terminate,1));
            }
            DODEBUG("[T%d-M%d] is fetching data| ", pidl4, getpid());
            if (exein_fetch_data(h, pidl4, data) == EXEIN_NOERR) {
                std::vector<std::size_t> shape = { EXEIN_BUFFES_SIZE };
                auto input_data = xt::adapt((short unsigned int*)data, EXEIN_BUFFES_SIZE, xt::no_ownership(), shape);

                if (old_hooks.count(pidl4) == 0) {
                    old_hooks[pidl4] = input_data;
                } else {
                    if (old_hooks[pidl4] == input_data) {
                        continue;
                    } else {
                        old_hooks[pidl4] = input_data;
                    }
                }

                int nonzero = EXEIN_BUFFES_SIZE - std::count(input_data.begin(), input_data.end(), 0);
                if (nonzero < std::stoi(model_params["window_size"])) {
                    continue;
                }

#ifdef DEBUG
                std::cout << "--------------------------------" << "\n";
                std::cout << pidl4 << " input data: " << input_data << '\n';
                std::cout << "prediction for: " << pidl4 << std::endl;
#endif

                std::tie(predictions, errors, signal_) = mle_player(interpreter.get(), input_data, pidl4, model_params, predictions, errors);

#ifdef DEBUG
                for (auto e: errors) {
                    std::cout << e.first << ": " << e.second << "\n";
                }
#endif

                if (signal_) {
                    std::cout << "Block process: " << pidl4 << "\n";
                    exein_block_process(h, pidl4, secret, tag);
                }
            } else {
                DODEBUG("[T%d-M%d] fetch_data timeout\n", pidl4, getpid() );
	    }
        } else {// Master process
            usleep(300);
            if ((tmppid=dequeue(&addpid,1))!=0) {
                if ((tmppid2=fork())==0) {
                    DODEBUG("\nNew pid appeared = [T%d-M%d] \n", tmppid2, tmppid);
                    pidl4=tmppid;
                }
            }
        }
    }

    exein_agent_stop(h);
    return 0;
}

