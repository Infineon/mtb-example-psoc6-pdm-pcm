/******************************************************************************
* File Name:   main.c
*
* Description: This is the source code for the PDM PCM Example
*              for ModusToolbox.
*
* Related Document: See Readme.md
*
*
*******************************************************************************
* (c) (2019), Cypress Semiconductor Corporation. All rights reserved.
*******************************************************************************
* This software, including source code, documentation and related materials
* ("Software"), is owned by Cypress Semiconductor Corporation or one of its
* subsidiaries ("Cypress") and is protected by and subject to worldwide patent
* protection (United States and foreign), United States copyright laws and
* international treaty provisions. Therefore, you may use this Software only
* as provided in the license agreement accompanying the software package from
* which you obtained this Software ("EULA").
*
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software source
* code solely for use in connection with Cypress's integrated circuit products.
* Any reproduction, modification, translation, compilation, or representation
* of this Software except as specified above is prohibited without the express
* written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer of such
* system or application assumes all risk of such use and in doing so agrees to
* indemnify Cypress against all liability.
*******************************************************************************/

#include "cy_pdl.h"
#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

#include "stdlib.h"

/*******************************************************************************
* Macros
********************************************************************************/
/* Trigger level configured in the PDM/PCM */
#define PDM_PCM_FIFO_TRG_LVL        128u
/* Define how many samples in a frame */
#define FRAME_SIZE                  (4*PDM_PCM_FIFO_TRG_LVL)
/* Noise threshold hysteresis */
#define THRESHOLD_HYSTERESIS        3u
/* Volume ratio for noise and print purposes */
#define VOLUME_RATIO                (1024u)

/*******************************************************************************
* Function Prototypes
********************************************************************************/
void button_isr_handler(void *callback_arg, cyhal_gpio_event_t event);
void pdm_pcm_isr_handler(void);

/*******************************************************************************
* Global Variables
********************************************************************************/
/* Interrupt flags */
volatile bool button_flag = false;
volatile bool pdm_pcm_flag = false;

/* Volume variables */
uint32_t volume = 0;
uint32_t num_samples = 0;
uint32_t noise_threshold = THRESHOLD_HYSTERESIS;

const cy_stc_sysint_t pdm_pcm_isr_cfg = {
#if CY_IP_MXAUDIOSS_INSTANCES == 1
    .intrSrc = (IRQn_Type) audioss_interrupt_pdm_IRQn,
#else
    .intrSrc = (IRQn_Type) audioss_0_interrupt_pdm_IRQn,
#endif
    .intrPriority = CYHAL_ISR_PRIORITY_DEFAULT
};

/*******************************************************************************
* Function Name: main
********************************************************************************
* Summary:
* The main function for Cortex-M4 CPU does the following:
*  Initialization:
*  - Initializes all the hardware blocks
*  Do forever loop:
*  - Check if PDM/PCM flag is set. If yes, report the current volume
*  - Update the LED status based on the volume and the noise threshold
*  - Check if the User Button was pressed. If yes, reset the noise threshold
*
* Parameters:
*  void
*
* Return:
*  int
*
*******************************************************************************/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    /* Initialize retarget-io to use the debug UART port */
    cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX, CY_RETARGET_IO_BAUDRATE);

    /* Initialize the User LED */
    cyhal_gpio_init((cyhal_gpio_t) CYBSP_USER_LED, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);

    /* Initialize the User Button */
    cyhal_gpio_init((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    cyhal_gpio_enable_event((cyhal_gpio_t) CYBSP_USER_BTN, CYHAL_GPIO_IRQ_FALL, CYHAL_ISR_PRIORITY_DEFAULT, true);
    cyhal_gpio_register_callback((cyhal_gpio_t) CYBSP_USER_BTN, button_isr_handler, NULL);

    /* Initialize the PDM/PCM interrupt (PDL) */
    Cy_SysInt_Init(&pdm_pcm_isr_cfg, pdm_pcm_isr_handler);
    NVIC_EnableIRQ(pdm_pcm_isr_cfg.intrSrc);

    /* Initialize the PDM/PCM block (PDL) */
    Cy_PDM_PCM_Init(CYBSP_PDM_PCM_HW, &CYBSP_PDM_PCM_config);
    Cy_PDM_PCM_ClearFifo(CYBSP_PDM_PCM_HW);
    Cy_PDM_PCM_Enable(CYBSP_PDM_PCM_HW);
    
    /* \x1b[2J\x1b[;H - ANSI ESC sequence for clear screen */
    printf("\x1b[2J\x1b[;H");

    printf("****************** \
    PDM/PCM Example \
    ****************** \r\n\n");

    for(;;)
    {
        /* Check if any microphone has data to process */
        if (pdm_pcm_flag)
        {
            /* Clear the PDM/PCM flag */
            pdm_pcm_flag = 0;

            /* Calculate the volume by summing the absolute value of all the 
             * data stored in the PDM/PCM FIFO */
            for (uint32_t index = 0; index < PDM_PCM_FIFO_TRG_LVL; index++)
            {
                volume += abs((int32_t) Cy_PDM_PCM_ReadFifo(CYBSP_PDM_PCM_HW));
            }

            /* Add to the number of samples */
            num_samples += PDM_PCM_FIFO_TRG_LVL;

            /* Clear the PDM/PCM interrupt */
            Cy_PDM_PCM_ClearInterrupt(CYBSP_PDM_PCM_HW, CY_PDM_PCM_INTR_RX_TRIGGER);

            /* Re-enable PDM/PCM ISR */
            NVIC_EnableIRQ(pdm_pcm_isr_cfg.intrSrc);

            /* Check if ready to process an entire frame */
            if (num_samples >= FRAME_SIZE)
            {
                /* Prepare line to report the volume */
                printf("\n\r");

                /* Report the volume */
                for (uint32_t index = 0; index < (volume/VOLUME_RATIO); index++)
                {
                    printf("-");
                }

                /* Turn ON the LED when the volume is higher than the threshold */
                if ((volume/VOLUME_RATIO) > noise_threshold)
                {
                    cyhal_gpio_write((cyhal_gpio_t) CYBSP_USER_LED, CYBSP_LED_STATE_ON);
                }
                else
                {
                    cyhal_gpio_write((cyhal_gpio_t) CYBSP_USER_LED, CYBSP_LED_STATE_OFF);
                }

                /* Reset the number of samples and volume */
                num_samples = 0;
                volume = 0;               
            }
        }

        /* Reset the noise threshold if User Button is pressed */
        if (button_flag)
        {
            /* Reset button flag */
            button_flag = false;

            /* Get the current volume and add a hysteresis as the new threshold */
            noise_threshold = (volume/VOLUME_RATIO) + THRESHOLD_HYSTERESIS;

            /* Report the new noise threshold over UART */
            printf("Noise threshold: %lu\n\r", (uint32_t) noise_threshold);
        }

    }
}

/*******************************************************************************
* Function Name: button_isr_handler
********************************************************************************
* Summary:
* Button ISR handler. Set a flag to be processed in the main loop.
*
* Parameters:
*  callback_arg: not used
*  event: event that occured
*
*******************************************************************************/
void button_isr_handler(void *callback_arg, cyhal_gpio_event_t event)
{
    (void) callback_arg;
    (void) event;

    button_flag = true;
}

/*******************************************************************************
* Function Name: pdm_pcm_isr_handler
********************************************************************************
* Summary:
* PDM/PCM ISR handler. Set a flag to be processed in the main loop.
*
*******************************************************************************/
void pdm_pcm_isr_handler(void)
{
    pdm_pcm_flag = true;

    /* Disable PDM/PCM ISR to avoid multiple calls to this ISR */
    NVIC_DisableIRQ(pdm_pcm_isr_cfg.intrSrc);
}

/* [] END OF FILE */
