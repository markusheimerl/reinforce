#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "net.h"
#include "quad.h"

#define DT_PHYSICS (1.0/1000.0)
#define DT_CONTROL (1.0/60.0)
#define DT_RENDER (1.0/24.0)

#define STATE_DIM 6      // 3 accel + 3 gyro
#define ACTION_DIM 8      // 4 means + 4 stds
#define MAX_STEPS 256
#define NUM_ROLLOUTS 128

#define GAMMA 0.999
#define MAX_STD 3.0
#define MIN_STD 1e-5

#define MAX_MEAN (OMEGA_MAX - 4.0 * MAX_STD)
#define MIN_MEAN (OMEGA_MIN + 4.0 * MAX_STD)

typedef struct {
    double** states;    // [MAX_STEPS][STATE_DIM]
    double** actions;   // [MAX_STEPS][ACTION_DIM]
    double* rewards;    // [MAX_STEPS]
    int length;
} Rollout;

Rollout* create_rollout() {
    Rollout* r = (Rollout*)malloc(sizeof(Rollout));
    r->states = (double**)malloc(MAX_STEPS * sizeof(double*));
    r->actions = (double**)malloc(MAX_STEPS * sizeof(double*));
    r->rewards = (double*)malloc(MAX_STEPS * sizeof(double));
    
    for(int i = 0; i < MAX_STEPS; i++) {
        r->states[i] = (double*)malloc(STATE_DIM * sizeof(double));
        r->actions[i] = (double*)malloc(ACTION_DIM * sizeof(double));
    }
    return r;
}

void free_rollout(Rollout* r) {
    for(int i = 0; i < MAX_STEPS; i++) {
        free(r->states[i]);
        free(r->actions[i]);
    }
    free(r->states);
    free(r->actions);
    free(r->rewards);
    free(r);
}

double squash(double x, double min, double max) { 
    return ((max + min) / 2.0) + ((max - min) / 2.0) * tanh(x); 
}

double dsquash(double x, double min, double max) { 
    return ((max - min) / 2.0) * (1.0 - tanh(x) * tanh(x)); 
}

double compute_reward(Quad q) {
    // Calculate distance to target position (0, 1, 0)
    double distance = sqrt(
        pow(q.linear_position_W[0] - 0.0, 2) +  // x distance from 0
        pow(q.linear_position_W[1] - 1.0, 2) +  // y distance from 1
        pow(q.linear_position_W[2] - 0.0, 2)    // z distance from 0
    );
    
    // Exponential decay from 1.0 at distance=0 to ~0 at large distances
    return exp(-4.0 * distance);
}

void collect_rollout(Net* policy, Rollout* rollout) {
    Quad quad = create_quad(0.0, 1.0, 0.0);
    
    double t_physics = 0.0;
    double t_control = 0.0;
    rollout->length = 0;

    while(rollout->length < MAX_STEPS) {
        if (sqrt(
            pow(quad.linear_position_W[0], 2) +
            pow(quad.linear_position_W[1] - 1.0, 2) +
            pow(quad.linear_position_W[2], 2)) > 1.0) {
            break;
        }

        if (t_physics >= DT_PHYSICS) {
            update_quad(&quad, DT_PHYSICS);
            t_physics = 0.0;
        }
        
        if (t_control >= DT_CONTROL) {
            // Use only sensor readings as state
            memcpy(rollout->states[rollout->length], quad.linear_acceleration_B_s, 3 * sizeof(double));
            memcpy(rollout->states[rollout->length] + 3, quad.angular_velocity_B_s, 3 * sizeof(double));
            
            forward_net(policy, rollout->states[rollout->length]);
            
            // Sample actions from Gaussian distribution
            for(int i = 0; i < 4; i++) {
                double mean = squash(policy->layers[policy->num_layers-1].values[i], MIN_MEAN, MAX_MEAN);
                double std = squash(policy->layers[policy->num_layers-1].values[i + 4], MIN_STD, MAX_STD);

                double u1 = (double)rand()/RAND_MAX;
                double u2 = (double)rand()/RAND_MAX;
                double noise = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
                
                rollout->actions[rollout->length][i] = mean + std * noise;
                quad.omega_next[i] = rollout->actions[rollout->length][i];
            }
            
            rollout->rewards[rollout->length] = compute_reward(quad);
            rollout->length++;
            t_control = 0.0;
        }
        
        t_physics += DT_PHYSICS;
        t_control += DT_PHYSICS;
    }
    
    // Compute discounted returns
    double G = 0.0;
    for(int i = rollout->length-1; i >= 0; i--) {
        rollout->rewards[i] = G = rollout->rewards[i] + GAMMA * G;
    }
}

// ∇J(θ) = E[∇_θ log π_θ(a|s) * R] ≈ 1/N Σ_t [∇_θ log π_θ(a_t|s_t) * R_t]
// Where:
// J(θ) - Policy objective function
// π_θ(a|s) - Gaussian policy parameterized by θ (network weights)
// R_t - Discounted return from time step t
void update_policy(Net* policy, Rollout* rollout) {
    double output_gradient[ACTION_DIM];
    
    for(int t = 0; t < rollout->length; t++) {
        forward_net(policy, rollout->states[t]);
        
        for(int i = 0; i < 4; i++) {
            // Network outputs raw parameters before squashing
            double mean_raw = policy->layers[policy->num_layers-1].values[i];
            double std_raw = policy->layers[policy->num_layers-1].values[i + 4];
            
            // Squashed parameters using tanh-based scaling
            // μ = ((MAX+MIN)/2) + ((MAX-MIN)/2)*tanh(mean_raw)
            // σ = ((MAX_STD+MIN_STD)/2) + ((MAX_STD-MIN_STD)/2)*tanh(std_raw)
            double mean = squash(mean_raw, MIN_MEAN, MAX_MEAN);
            double std_val = squash(std_raw, MIN_STD, MAX_STD);
            
            // Sampled action and its deviation from mean
            double action = rollout->actions[t][i];
            double delta = action - mean;

            // Gradient for mean parameter:
            // ∇_{μ_raw} log π = [ (a - μ)/σ² ] * dμ/dμ_raw
            // Where:
            // (a - μ)/σ² = derivative of log N(a; μ, σ²) w.r.t μ
            // dμ/dμ_raw = derivative of squashing function (dsquash)
            output_gradient[i] = -(delta / (std_val * std_val)) * 
                dsquash(mean_raw, MIN_MEAN, MAX_MEAN) * 
                rollout->rewards[t];

            // Gradient for standard deviation parameter:
            // ∇_{σ_raw} log π = [ ( (a-μ)^2 - σ² ) / σ³ ] * dσ/dσ_raw
            // Where:
            // ( (a-μ)^2 - σ² ) / σ³ = derivative of log N(a; μ, σ²) w.r.t σ
            // dσ/dσ_raw = derivative of squashing function (dsquash)
            output_gradient[i + 4] = -((delta * delta - std_val * std_val) / 
                (std_val * std_val * std_val)) * 
                dsquash(std_raw, MIN_STD, MAX_STD) * 
                rollout->rewards[t];
        }

        // Backpropagate gradients through network
        // Negative sign converts gradient ascent (policy improvement) 
        // to gradient descent (standard optimization framework)
        backward_net(policy, output_gradient);
    }
}

int main(int argc, char** argv) {
    if(argc != 2 && argc != 3) {
        printf("Usage: %s <num_epochs> [initial_weights.bin]\n", argv[0]);
        return 1;
    }

    srand(time(NULL) ^ getpid());
    
    static const int layer_sizes[] = {STATE_DIM, 64, ACTION_DIM};
    Net* net = (argc == 3) ? load_net(argv[2]) : create_net(3, layer_sizes, 5e-7);
    
    Rollout* rollouts[NUM_ROLLOUTS];
    for(int r = 0; r < NUM_ROLLOUTS; r++) rollouts[r] = create_rollout();

    int epochs = atoi(argv[1]);
    double best_return = -1e30;
    double theoretical_max = (1.0 - pow(GAMMA + 1e-15, MAX_STEPS))/(1.0 - (GAMMA + 1e-15));
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
    
    for(int epoch = 0; epoch < epochs; epoch++) {
        double sum_returns = 0.0;
        for(int r = 0; r < NUM_ROLLOUTS; r++) {
            collect_rollout(net, rollouts[r]);
            sum_returns += rollouts[r]->rewards[0];
        }
        
        for(int r = 0; r < NUM_ROLLOUTS; r++) {
            update_policy(net, rollouts[r]);
        }

        double mean_return = sum_returns / NUM_ROLLOUTS;
        best_return = fmax(mean_return, best_return);

        struct timeval now;
        gettimeofday(&now, NULL);
        double elapsed = (now.tv_sec - start_time.tv_sec) + (now.tv_usec - start_time.tv_usec) / 1e6;
        
        printf("epoch %d/%d | Return: %.2f/%.2f (%.1f%%) | Best: %.2f | Rate: %.3f %%/s\n", 
            epoch+1, epochs,
            mean_return, theoretical_max, 
            (mean_return/theoretical_max) * 100.0, best_return,
            ((best_return/theoretical_max) * 100.0 / elapsed));
    }

    char filename[64];
    time_t current_time = time(NULL);
    strftime(filename, sizeof(filename), "%Y%m%d_%H%M%S_policy.bin", localtime(&current_time));
    save_net(filename, net);

    for(int r = 0; r < NUM_ROLLOUTS; r++) free_rollout(rollouts[r]);
    free_net(net);
    return 0;
}