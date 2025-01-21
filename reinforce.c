#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "grad/grad.h"
#include "sim/sim.h"

#define DT_PHYSICS (1.0/1000.0)
#define DT_CONTROL (1.0/60.0)
#define MAX_DISTANCE 2.0
#define MAX_VELOCITY 5.0
#define MAX_ANGULAR_VELOCITY 5.0

#define STATE_DIM 12
#define HIDDEN_DIM 64
#define ACTION_DIM 8

#define MAX_STEPS 1000
#define NUM_ROLLOUTS 128

#define ALPHA 1e-9
#define MAX_STD 2.0
#define MIN_STD 1e-6

const double TARGET_POS[3] = {0.0, 1.0, 0.0};

void get_state(Quad* q, double* state) {
    memcpy(state, q->linear_position_W, 3 * sizeof(double));
    memcpy(state + 3, q->linear_velocity_W, 3 * sizeof(double));
    memcpy(state + 6, q->angular_velocity_B, 3 * sizeof(double));
    state[9] = q->R_W_B[0];
    state[10] = q->R_W_B[4];
    state[11] = q->R_W_B[8];
}

double compute_reward(Quad* q) {
    // L2 norm of position error: ‖p - p_target‖₂
    double pos_error = 0.0;
    for(int i = 0; i < 3; i++) {
        pos_error += pow(q->linear_position_W[i] - TARGET_POS[i], 2);
    }
    pos_error = sqrt(pos_error);
    
    // L2 norm of velocity: ‖v‖₂
    double vel_magnitude = 0.0;
    for(int i = 0; i < 3; i++) {
        vel_magnitude += pow(q->linear_velocity_W[i], 2);
    }
    vel_magnitude = sqrt(vel_magnitude);
    
    // L2 norm of angular velocity: ‖ω‖₂
    double ang_vel_magnitude = 0.0;
    for(int i = 0; i < 3; i++) {
        ang_vel_magnitude += pow(q->angular_velocity_B[i], 2);
    }
    ang_vel_magnitude = sqrt(ang_vel_magnitude);
    
    // Orientation error: |1 - R₂₂|
    double orientation_error = fabs(1.0 - q->R_W_B[4]);
    
    // Combined weighted error
    double total_error = (pos_error * 2.0) +
                        (vel_magnitude * 1.0) +
                        (ang_vel_magnitude * 0.5) +
                        (orientation_error * 2.0);
    
    return exp(-total_error);
}

bool is_terminated(Quad* q) {
    double dist = 0.0, vel = 0.0, ang_vel = 0.0;
    for(int i = 0; i < 3; i++) {
        dist += pow(q->linear_position_W[i] - TARGET_POS[i], 2);
        vel += pow(q->linear_velocity_W[i], 2);
        ang_vel += pow(q->angular_velocity_B[i], 2);
    }
    return sqrt(dist) > MAX_DISTANCE || sqrt(vel) > MAX_VELOCITY || 
           sqrt(ang_vel) > MAX_ANGULAR_VELOCITY || q->R_W_B[4] < 0.0;
}

int collect_rollout(Sim* sim, Net* policy, double** act, double** states, double** actions, double* rewards) {
    // Initialize quadcopter with small random offset from target
    reset_quad(sim->quad, 
        TARGET_POS[0] + ((double)rand()/RAND_MAX - 0.5) * 0.2,
        TARGET_POS[1] + ((double)rand()/RAND_MAX - 0.5) * 0.2, 
        TARGET_POS[2] + ((double)rand()/RAND_MAX - 0.5) * 0.2
    );
    
    double t_physics = 0.0, t_control = 0.0;
    int steps = 0;
    
    while(steps < MAX_STEPS && !is_terminated(sim->quad)) {
        update_quad(sim->quad, DT_PHYSICS);
        t_physics += DT_PHYSICS;
        
        if(t_control <= t_physics) {
            get_state(sim->quad, states[steps]);
            fwd(policy, states[steps], act);
            
            // For each action dimension:
            for(int i = 0; i < 4; i++) {
                // 1. Get standard deviation using squash function:
                // σ = ((max + min)/2) + ((max - min)/2) * tanh(x)
                double std = squash(act[4][i + 4], MIN_STD, MAX_STD);
                
                // 2. Compute safe bounds for mean to ensure x = μ ± 4σ stays within [OMEGA_MIN, OMEGA_MAX]
                // OMEGA_MIN ≤ μ - 4σ and μ + 4σ ≤ OMEGA_MAX
                double safe_margin = 4.0 * std;  // 99.994% of samples within ±4σ
                double mean_min = OMEGA_MIN + safe_margin;
                double mean_max = OMEGA_MAX - safe_margin;
                
                // 3. Get mean using squash with dynamic bounds
                double mean = squash(act[4][i], mean_min, mean_max);
                
                // 4. Sample from N(μ, σ²) using Box-Muller transform:
                // If U₁,U₂ ~ Uniform(0,1)
                // Then √(-2ln(U₁))cos(2πU₂) ~ N(0,1)
                double u1 = (double)rand()/RAND_MAX;
                double u2 = (double)rand()/RAND_MAX;
                double noise = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
                
                // 5. Apply sampled action: x = μ + σz where z ~ N(0,1)
                actions[steps][i] = mean + std * noise;
                sim->quad->omega_next[i] = actions[steps][i];
            }
            
            rewards[steps] = compute_reward(sim->quad);
            steps++;
            t_control += DT_CONTROL;
        }
    }
    
    return steps;
}

void update_policy(Net* policy, double** states, double** actions, double* rewards, int steps, double** act, double** grad) {
    // 1. Compute mean and standard deviation of rewards for normalization
    // μ = (1/T)∑ᵢrᵢ
    double mean = 0.0;
    for(int t = 0; t < steps; t++) {
        mean += rewards[t];
    }
    mean /= steps;
    
    // σ² = (1/T)∑ᵢ(rᵢ - μ)²
    double variance = 0.0;
    for(int t = 0; t < steps; t++) {
        double diff = rewards[t] - mean;
        variance += diff * diff;
    }
    variance /= steps;
    double std = sqrt(variance + 1e-8);
    
    for(int t = 0; t < steps; t++) {
        fwd(policy, states[t], act);
        
        // 2. Normalize rewards: r̂ = (r - μ)/σ
        double normalized_reward = (rewards[t] - mean) / std;
        
        for(int i = 0; i < 4; i++) {
            // 3. Get policy distribution parameters
            double std = squash(act[4][i + 4], MIN_STD, MAX_STD);
            double safe_margin = 4.0 * std;
            double mean_min = OMEGA_MIN + safe_margin;
            double mean_max = OMEGA_MAX - safe_margin;
            double mean = squash(act[4][i], mean_min, mean_max);
            
            // 4. Compute normalized action: z = (x - μ)/σ
            double z = (actions[t][i] - mean) / std;
            
            // 5. Compute log probability: log(p(x)) = -½(log(2π) + 2log(σ) + z²)
            double log_prob = -0.5 * (1.8378770664093453 + 2.0 * log(std) + z * z);
            
            // 6. Compute entropy: H = ½(log(2πe) + 2log(σ))
            double entropy = 0.5 * (2.837877066 + 2.0 * log(std));
            
            // 7. Compute gradients for mean
            // ∂log_prob/∂μ = z/σ
            double dmean = z / std;
            grad[4][i] = (normalized_reward * log_prob + ALPHA * entropy) * 
                        dmean * dsquash(act[4][i], mean_min, mean_max);
            
            // 8. Compute gradients for standard deviation
            // ∂log_prob/∂σ = (z² - 1)/σ + (z/σ)(-4∂μ/∂σ)
            double dstd_direct = (z * z - 1.0) / std;
            double dmean_dstd = -4.0 * dsquash(act[4][i], mean_min, mean_max);
            double dstd = dstd_direct + (z / std) * dmean_dstd;
            
            grad[4][i + 4] = (normalized_reward * log_prob * dstd + ALPHA * (1.0 / std)) * 
                            dsquash(act[4][i + 4], MIN_STD, MAX_STD);
        }
        
        bwd(policy, act, grad);
    }
}

int main(int argc, char** argv) {
    if(argc != 2 && argc != 3) {
        printf("Usage: %s <num_iterations> [initial_weights.bin]\n", argv[0]);
        return 1;
    }

    srand(time(NULL));
    
    Net* net;
    if(argc == 3) {
        net = load_weights(argv[2], adamw);
    } else {
        int layers[] = {STATE_DIM, HIDDEN_DIM, HIDDEN_DIM, HIDDEN_DIM, ACTION_DIM};
        net = init_net(5, layers, adamw);
    }
    net->lr = 5e-5;
    
    Sim* sim = init_sim("", false);
    double** act = malloc(5 * sizeof(double*));
    double** grad = malloc(5 * sizeof(double*));
    
    for(int i = 0; i < 5; i++) {
        act[i] = malloc(net->sz[i] * sizeof(double));
        grad[i] = calloc(net->sz[i], sizeof(double));
    }

    int iterations = atoi(argv[1]);
    double best_mean_reward = -1e30;
    double initial_best = -1e30;
    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);
    
    for(int iter = 0; iter < iterations; iter++) {
        double sum_mean_rewards = 0.0;
        int total_steps = 0;

        for(int r = 0; r < NUM_ROLLOUTS; r++) {
            double* states[MAX_STEPS];
            double* actions[MAX_STEPS];
            double rewards[MAX_STEPS];
            
            for(int i = 0; i < MAX_STEPS; i++) {
                states[i] = malloc(STATE_DIM * sizeof(double));
                actions[i] = malloc(4 * sizeof(double));
            }

            int steps = collect_rollout(sim, net, act, states, actions, rewards);
            
            // Calculate mean reward for this rollout
            double rollout_mean = 0.0;
            for(int i = 0; i < steps; i++) {
                rollout_mean += rewards[i];
            }
            rollout_mean /= steps;
            sum_mean_rewards += rollout_mean;
            total_steps += steps;

            update_policy(net, states, actions, rewards, steps, act, grad);

            for(int i = 0; i < MAX_STEPS; i++) {
                free(states[i]);
                free(actions[i]);
            }
        }

        double mean_reward = sum_mean_rewards / NUM_ROLLOUTS;
        if(mean_reward > best_mean_reward) {
            best_mean_reward = mean_reward;
        }

        if(iter == 0) {
            initial_best = best_mean_reward;
        }

        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                        (current_time.tv_usec - start_time.tv_usec) / 1000000.0;
        
        double percentage = (mean_reward) * 100.0;  // Since rewards are already between 0 and 1
        double initial_percentage = initial_best * 100.0;
        double current_percentage = best_mean_reward * 100.0;
        double percentage_rate = (current_percentage - initial_percentage) / elapsed;

        printf("\rIter %d/%d | Return: %.2f (%.1f%%) | Best: %.2f | Rate: %.3f %%/s | lr: %.2e", 
               iter+1, iterations, mean_reward, percentage, 
               best_mean_reward, percentage_rate, net->lr);
        fflush(stdout);
    }
    printf("\n");

    char final_weights[64];
    strftime(final_weights, sizeof(final_weights), "%Y%m%d_%H%M%S_policy.bin", 
             localtime(&(time_t){time(NULL)}));
    save_weights(final_weights, net);
    printf("Final weights saved to: %s\n", final_weights);

    for(int i = 0; i < 5; i++) {
        free(act[i]);
        free(grad[i]);
    }
    free(act);
    free(grad);
    free_net(net);
    free_sim(sim);

    return 0;
}