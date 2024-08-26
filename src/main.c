#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>
#include <math.h>
#include <string.h>

#define PRIORITY 5
#define STACKSIZE 512
#define WINDOW_SIZE 1000

#define SPEED PWM_KHZ(20)   // Sets period to 3kHz with 100% duty cycle being 333,333 ns. 


#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

static const struct pwm_dt_spec pwm_28 = PWM_DT_SPEC_GET(DT_NODELABEL(pin28));
static uint32_t min_pulse_28 = DT_PROP(DT_NODELABEL(pin28), min_pulse);
// static uint32_t max_pulse_28 = DT_PROP(DT_NODELABEL(pin28), max_pulse);


static const struct pwm_dt_spec pwm_5 = PWM_DT_SPEC_GET(DT_NODELABEL(pin5));
static uint32_t min_pulse_5 = DT_PROP(DT_NODELABEL(pin5), min_pulse);
// static uint32_t max_pulse_29 = DT_PROP(DT_NODELABEL(pin29), max_pulse);

static const struct pwm_dt_spec pwm_2 = PWM_DT_SPEC_GET(DT_NODELABEL(pin2));
static uint32_t min_pulse_24 = DT_PROP(DT_NODELABEL(pin2), min_pulse);

static const struct pwm_dt_spec pwm_9 = PWM_DT_SPEC_GET(DT_NODELABEL(pin9));
static uint32_t min_pulse_9 = DT_PROP(DT_NODELABEL(pin9), min_pulse);


volatile static float angle = 130.0;
volatile static float commanded_current = 0.0;
volatile static float accel_profile;
volatile static float actual_radians;
volatile static float avg_step_cadance;
static float point_arr[3];

// Define queue
static K_THREAD_STACK_DEFINE(motor1_q_stack_area, STACKSIZE);
static struct k_work_q motor1_work_q = {0};

// Create queue struct
struct pwm_command {
    struct k_work work;
    float pwm;
} motor1_command;
    
/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels,
			     DT_SPEC_AND_COMMA)
};

struct adc_sequence sequence;
uint16_t buf;
float filteredEncoderReading = 0.0;

// Define mutex for sensor readings
K_FIFO_DEFINE(printk_fifo);
K_MUTEX_DEFINE(pot_adc_mutex);

// Define timers for control loops
K_TIMER_DEFINE(current_timer, NULL, NULL);
K_TIMER_DEFINE(pot_adc_timer, NULL, NULL);

int period = 20000000;
void motor_control(struct k_work *input)
{
    int ret;
    // Get command from queue
    struct pwm_command *motor1 = CONTAINER_OF(input, struct pwm_command, work);
    // Transform to range for esc (1-2ms)
    int pwm_val = (motor1->pwm / 4100) *  1000000 + 1000000; // units of ns
    // display
    printk("%d\n",pwm_val);
    // Set the motor pwms
    // ret = pwm_set_pulse_dt(&pwm_28, pwm_val);
    ret = pwm_set_dt(&pwm_28, period,pwm_val);
    if (ret < 0) {
        printk("Error %d: failed to set 0 pulse width for 28\n", ret);
    }
    // ret = pwm_set_pulse_dt(&pwm_5, pwm_val);
    ret = pwm_set_dt(&pwm_5, period,pwm_val);
    if (ret < 0) {
        printk("Error %d: failed to set 0 pulse width for 5\n", ret);
    }
    // ret = pwm_set_pulse_dt(&pwm_2, pwm_val);
    ret = pwm_set_dt(&pwm_2, period,pwm_val);
    if (ret < 0) {
        printk("Error %d: failed to set 0 pulse width for 2\n", ret);
    }
    // ret = pwm_set_pulse_dt(&pwm_9, pwm_val);
    ret = pwm_set_dt(&pwm_9, period,pwm_val);
    if (ret < 0) {
        printk("Error %d: failed to set 0 pulse width for 9\n", ret);
    }

}

/// Thread Definitions

void read_pot_adc(void)
{
    int err;

    sequence.buffer = &buf;
    sequence.buffer_size = sizeof(buf);
    

    float rawBuffer[WINDOW_SIZE] = {0}; 
    int currentIndex = 0; 

    k_work_queue_init(&motor1_work_q);         // init work q
    k_work_queue_start(&motor1_work_q, motor1_q_stack_area,K_THREAD_STACK_SIZEOF(motor1_q_stack_area), PRIORITY,NULL); // Start data queue 
    k_work_init(&motor1_command,motor_control);// init work item

    k_sleep(K_SECONDS(4));

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!device_is_ready(adc_channels[i].dev)) {
			printk("ADC controller device %s not ready\n",adc_channels[i].dev->name);
		}

		err = adc_channel_setup_dt(&adc_channels[i]);
		if (err < 0) {
			printk("Could not setup channel #%d (%d)\n", i, err);
		}
	}

    // ESC calibraiton sequence
    for(int i = 0; i < 1500;i++){
        k_timer_start(&pot_adc_timer, K_MSEC(1), K_NO_WAIT);
        if(i < 500){
            // Save to work queue
            motor1_command.pwm = 4100;
            // Submit to queue
            k_work_submit_to_queue(&motor1_work_q, &motor1_command.work);
        } else {
            // Save to work queue
            motor1_command.pwm = 4100 - (i-500) * 4.1;            
            // motor1_command.pwm = 0;

            // Submit to queue
            k_work_submit_to_queue(&motor1_work_q, &motor1_command.work);
        }
        k_timer_status_sync(&pot_adc_timer);
    }

	while (1) {
        // start timer, run at 1000Hz
        k_timer_start(&pot_adc_timer, K_MSEC(1), K_NO_WAIT);
        
        (void)adc_sequence_init_dt(&adc_channels[0], &sequence);
        // Read adc, assign to buf
        err = adc_read(adc_channels[0].dev, &sequence);
        if (err < 0) {
            printk("Could not read (%d)\n", err);
            continue;
        }
        // Save to work queue
        motor1_command.pwm = buf;
        // Submit to queue
        k_work_submit_to_queue(&motor1_work_q, &motor1_command.work);
		// check if timer is expired, wait if necessary
        k_timer_status_sync(&pot_adc_timer);
	}
}

K_THREAD_DEFINE(pot_id, STACKSIZE, read_pot_adc, NULL, NULL, NULL,
PRIORITY, 0, 0);

int main(void)
{
    usb_enable(NULL);
    return 0;

}


