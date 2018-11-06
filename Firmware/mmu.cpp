//mmu.cpp

#include "mmu.h"
#include "planner.h"
#include "language.h"
#include "lcd.h"
#include "uart2.h"
#include "temperature.h"
#include "Configuration_prusa.h"
#include "fsensor.h"
#include "cardreader.h"
#include "ultralcd.h"
#include "sound.h"
#include "printers.h"
#include <avr/pgmspace.h>

#define CHECK_FINDA ((IS_SD_PRINTING || is_usb_printing) && (mcode_in_progress != 600) && !saved_printing && e_active())

#define MMU_TODELAY 100
#define MMU_TIMEOUT 10
#define MMU_CMD_TIMEOUT 300000ul //5min timeout for mmu commands (except P0)
#define MMU_P0_TIMEOUT 3000ul //timeout for P0 command: 3seconds

#ifdef MMU_HWRESET
#define MMU_RST_PIN 76
#endif //MMU_HWRESET

bool mmu_enabled = false;

bool mmu_ready = false;

bool mmuFilamentMK3Moving = false;
bool mmuFSensorLoading = false;
bool singleLog = true;
int lastLoadedFilament = -10;

int8_t mmu_state = 0;
int8_t last_state = -10;

uint8_t mmu_cmd = 0;
bool ack_received = false;

uint8_t mmu_extruder = 0;

//! This variable probably has no meaning and is planed to be removed
uint8_t tmp_extruder = 0;

int8_t mmu_finda = -1;

int16_t mmu_version = -1;

int16_t mmu_buildnr = -1;

uint32_t mmu_last_request = 0;
uint32_t mmu_last_response = 0;

//clear rx buffer
void mmu_clr_rx_buf(void)
{
    while (fgetc(uart2io) >= 0);
}

//send command - puts
int mmu_puts_P(const char* str)
{
    mmu_clr_rx_buf();                          //clear rx buffer
    int r = fputs_P(str, uart2io);             //send command
    mmu_last_request = millis();
    return r;
}

//send command - printf
int mmu_printf_P(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    mmu_clr_rx_buf();                          //clear rx buffer
    int r = vfprintf_P(uart2io, format, args); //send command
    va_end(args);
    mmu_last_request = millis();
    return r;
}

//check 'sensing Filament at Boot' response
int8_t mmu_rx_echo(void)
{
    int8_t res = 0;
    switch (mmu_cmd)
    {
    case MMU_CMD_T0: // T0
        res = uart2_rx_str_P(PSTR("T0\n"));
        break;
    case MMU_CMD_T1: // T1
        res = uart2_rx_str_P(PSTR("T1\n"));
        break;
    case MMU_CMD_T2: // T2
        res = uart2_rx_str_P(PSTR("T2\n"));
        break;
    case MMU_CMD_T3: // T3
        res = uart2_rx_str_P(PSTR("T3\n"));
        break;
    case MMU_CMD_T4: // T4
        res = uart2_rx_str_P(PSTR("T4\n"));
        break;
    case MMU_CMD_L0: // L0
        res = uart2_rx_str_P(PSTR("L0\n"));
        break;
    case MMU_CMD_L1: // L1
        res = uart2_rx_str_P(PSTR("L1\n"));
        break;
    case MMU_CMD_L2: // L2
        res = uart2_rx_str_P(PSTR("L2\n"));
        break;
    case MMU_CMD_L3: // L3
        res = uart2_rx_str_P(PSTR("L3\n"));
        break;
    case MMU_CMD_L4: // L4
        res = uart2_rx_str_P(PSTR("L4\n"));
        break;
    case MMU_CMD_C0: // C0
        res = uart2_rx_str_P(PSTR("C0\n"));
        break;
    case MMU_CMD_U0: // U0
        res = uart2_rx_str_P(PSTR("U0\n"));
        break;
    case MMU_CMD_E0: // E0
        res = uart2_rx_str_P(PSTR("E0\n"));
        break;
    case MMU_CMD_E1: // E1
        res = uart2_rx_str_P(PSTR("E1\n"));
        break;
    case MMU_CMD_E2: // E2
        res = uart2_rx_str_P(PSTR("E2\n"));
        break;
    case MMU_CMD_E3: // E3
        res = uart2_rx_str_P(PSTR("E3\n"));
        break;
    case MMU_CMD_E4: // E4
        res = uart2_rx_str_P(PSTR("E4\n"));
        break;
    case MMU_CMD_R0: // R0
        res = uart2_rx_str_P(PSTR("R0\n"));
        break;
    }
    if (res == 0) puts_P(PSTR("MMU Didn't see CMD and ECHO"));
    if (res == 1) mmu_last_response = millis();
    return res;
}

//check 'ok' response
int8_t mmu_rx_ok(void)
{
    int8_t res = uart2_rx_str_P(PSTR("ok\n"));
    if (res == 1) mmu_last_response = millis();
    return res;
}

//check ' not ok' response
int8_t mmu_rx_not_ok(void)
{
    int8_t res = uart2_rx_str_P(PSTR("not_ok\n"));
    if (res == 1) mmu_last_response = millis();
    return res;
}

//check 'start' response
int8_t mmu_rx_start(void)
{
    int8_t res = uart2_rx_str_P(PSTR("start\n"));
    if (res == 1) mmu_last_response = millis();
    return res;
}

//initialize mmu2 unit - first part - should be done at begining of startup process
void mmu_init(void)
{
#ifdef MMU_HWRESET
    digitalWrite(MMU_RST_PIN, HIGH);
    pinMode(MMU_RST_PIN, OUTPUT);              //setup reset pin
#endif //MMU_HWRESET
    uart2_init();                              //init uart2
    _delay_ms(10);                             //wait 10ms for sure
    mmu_reset();                               //reset mmu (HW or SW), do not wait for response
    mmu_state = -1;
}

//mmu main loop - state machine processing
/**
 * MMU States
 * -1 >> -2 MMURX Start, respond with S1 (30s timeout to disabled state)
 * -2 >> -3 MMURX ok, respond with S2
 * -3 >> -4 MMURX ok, respond with P0(READ FINDA) if MK3 and goto -4
 *       -5           respond with M1(MODE STEALTH) if MK2.5 and goto -5
 * -4 >>  1 MMURX ok, mmu_ready
 * -5 >>  1 MMURX ok, mmu_ready
 *  1 >>  ? MMU CMD Request from MK3
 *  2 >>  1 MMURX ok, Finda State
 *  3 >>  1 MMURX ok, mmu commands response
 * 10 >>  3 MMUECHO, confirm receipt of cmd (timeout 500ms to resend)
 */

void mmu_loop(void)
{
    int filament = 0;
#ifdef MMU_DEBUG
    if (last_state != mmu_state) printf_P(PSTR("MMU loop, state=%d\n"), mmu_state);
#endif //MMU_DEBUG
    last_state = mmu_state;
    switch (mmu_state)
    {
    case 0:
        return;
    case -1:
        if (mmu_rx_start() > 0)
        {
#ifdef MMU_DEBUG
            puts_P(PSTR("MMU => 'start'"));
            puts_P(PSTR("MMU <= 'S1'"));
#endif //MMU_DEBUG
            mmu_puts_P(PSTR("S1\n")); //send 'read version' request
            mmu_state = -2;
        }
        else if (millis() > 30000) //30sec after reset disable mmu
        {
            puts_P(PSTR("MMU not responding - DISABLED"));
            mmu_state = 0;
        }
        return;
    case -2:
        if (mmu_rx_ok() > 0)
        {
            fscanf_P(uart2io, PSTR("%u"), &mmu_version); //scan version from buffer
#ifdef MMU_DEBUG
            printf_P(PSTR("MMU => '%dok'\n"), mmu_version);
            puts_P(PSTR("MMU <= 'S2'"));
#endif //MMU_DEBUG
            mmu_puts_P(PSTR("S2\n")); //send 'read buildnr' request
            mmu_state = -3;
        }
        return;
    case -3:
        if (mmu_rx_ok() > 0)
        {
            fscanf_P(uart2io, PSTR("%u"), &mmu_buildnr); //scan buildnr from buffer
#ifdef MMU_DEBUG
            printf_P(PSTR("MMU => '%dok'\n"), mmu_buildnr);
#endif //MMU_DEBUG
            bool version_valid = mmu_check_version();
            if (!version_valid) mmu_show_warning();
            else puts_P(PSTR("MMU version valid"));

            if ((PRINTER_TYPE == PRINTER_MK3) || (PRINTER_TYPE == PRINTER_MK3_SNMM))
            {
#ifdef MMU_DEBUG
                puts_P(PSTR("MMU <= 'P0'"));
#endif //MMU_DEBUG
                mmu_puts_P(PSTR("P0\n")); //send 'read finda' request
                mmu_state = -4;
            }
            else
            {
#ifdef MMU_DEBUG
                puts_P(PSTR("MMU <= 'M1'"));
#endif //MMU_DEBUG
                mmu_puts_P(PSTR("M1\n")); //set mmu mode to stealth
                mmu_state = -5;
            }

        }
        return;
    case -5:
        if (mmu_rx_ok() > 0)
        {
#ifdef MMU_DEBUG
            puts_P(PSTR("MMU <= 'P0'"));
#endif //MMU_DEBUG
            mmu_puts_P(PSTR("P0\n")); //send 'read finda' request
            mmu_state = -4;
        }
        return;
    case -4:
        if (mmu_rx_ok() > 0)
        {
            fscanf_P(uart2io, PSTR("%hhu"), &mmu_finda); //scan finda from buffer
#ifdef MMU_DEBUG
            printf_P(PSTR("MMU => '%dok'\n"), mmu_finda);
#endif //MMU_DEBUG
            puts_P(PSTR("MMU - ENABLED"));
            mmu_enabled = true;
            mmu_state = 1;
        }
        return;
    case 1:
        if (mmu_cmd && !ack_received) //command request ?
        {
            switch (mmu_cmd) {
            case MMU_CMD_T0:
                mmu_puts_P(PSTR("T0\n"));
                break;
            case MMU_CMD_T1:
                mmu_puts_P(PSTR("T1\n"));
                break;
            case MMU_CMD_T2:
                mmu_puts_P(PSTR("T2\n"));
                break;
            case MMU_CMD_T3:
                mmu_puts_P(PSTR("T3\n"));
                break;
            case MMU_CMD_T4:
                mmu_puts_P(PSTR("T4\n"));
                break;
            case MMU_CMD_L0:
                mmu_puts_P(PSTR("L0\n"));
                break;
            case MMU_CMD_L1:
                mmu_puts_P(PSTR("L1\n"));
                break;
            case MMU_CMD_L2:
                mmu_puts_P(PSTR("L2\n"));
                break;
            case MMU_CMD_L3:
                mmu_puts_P(PSTR("L3\n"));
                break;
            case MMU_CMD_L4:
                mmu_puts_P(PSTR("L4\n"));
                break;
            case MMU_CMD_C0:
                mmu_puts_P(PSTR("C0\n"));
                break;
            case MMU_CMD_U0:
                mmu_puts_P(PSTR("U0\n"));
                break;
            case MMU_CMD_E0:
                mmu_puts_P(PSTR("E0\n"));
                break;
            case MMU_CMD_E1:
                mmu_puts_P(PSTR("E1\n"));
                break;
            case MMU_CMD_E2:
                mmu_puts_P(PSTR("E2\n"));
                break;
            case MMU_CMD_E3:
                mmu_puts_P(PSTR("E3\n"));
                break;
            case MMU_CMD_E4:
                mmu_puts_P(PSTR("E4\n"));
                break;
            case MMU_CMD_R0:
                mmu_puts_P(PSTR("R0\n"));
                break;
            }
            mmu_state = 10;
        } else if (mmu_cmd && ack_received) {
            ack_received = false;
            if ((mmu_cmd >= MMU_CMD_T0) && (mmu_cmd <= MMU_CMD_T4))
            {
                filament = mmu_cmd - MMU_CMD_T0;
                if (lastLoadedFilament != filament) {
                    printf_P(PSTR("MMU <= 'T%d'\n"), filament);
                    mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                    //mmu_printf_P(PSTR("T%d\n"), filament);
                    mmu_state = 3; // wait for response
                    fsensor_enable();  // RMM:TODO Not sure if this is the best place to ensure enabled
                    fsensor_autoload_check_start();
                    mmuFSensorLoading = true;
                    fsensor_autoload_enabled = true;
                    mmuFilamentMK3Moving = false;
                }
                lastLoadedFilament = filament;
            }
            else if ((mmu_cmd >= MMU_CMD_L0) && (mmu_cmd <= MMU_CMD_L4))
            {
                filament = mmu_cmd - MMU_CMD_L0;
                printf_P(PSTR("MMU <= 'L%d'\n"), filament);
                mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                //mmu_printf_P(PSTR("L%d\n"), filament);
                mmu_state = 3; // wait for response
            }
            else if (mmu_cmd == MMU_CMD_C0)
            {
                delay(100);
                printf_P(PSTR("MMU <= 'C0'\n"));
                //mmu_puts_P(PSTR("C0\n")); //send 'continue loading'
                mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                mmu_state = 3;
            }
            else if (mmu_cmd == MMU_CMD_U0)
            {
                printf_P(PSTR("MMU <= 'U0'\n"));
                //mmu_puts_P(PSTR("U0\n")); //send 'unload current filament'
                mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                lastLoadedFilament = -10;
                mmu_state = 3;
            }
            else if ((mmu_cmd >= MMU_CMD_E0) && (mmu_cmd <= MMU_CMD_E4))
            {
                int filament = mmu_cmd - MMU_CMD_E0;
                printf_P(PSTR("MMU <= 'E%d'\n"), filament);
                //mmu_printf_P(PSTR("E%d\n"), filament); //send eject filament
                mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                mmu_state = 3; // wait for response
            }
            else if (mmu_cmd == MMU_CMD_R0)
            {
                printf_P(PSTR("MMU <= 'R0'\n"));
                //mmu_puts_P(PSTR("R0\n")); //send recover after eject
                mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
                mmu_state = 3; // wait for response
            }
            mmu_cmd = 0;
        }
        else if ((mmu_last_response + 500) < millis()) //request every 500ms
        {
            mmu_puts_P(PSTR("P0\n")); //send 'read finda' request
            mmu_state = 2;
        }
        return;
    case 2: //response to command P0
        if (mmu_rx_ok() > 0)
        {
            fscanf_P(uart2io, PSTR("%hhu"), &mmu_finda); //scan finda from buffer
            //printf_P(PSTR("MMU => '%dok'\n"), mmu_finda);
            if (!mmu_finda && CHECK_FINDA && fsensor_enabled) {
                fsensor_stop_and_save_print();
                enquecommand_front_P(PSTR("FSENSOR_RECOVER")); //then recover
                if (lcd_autoDeplete) enquecommand_front_P(PSTR("M600 AUTO")); //save print and run M600 command
                else enquecommand_front_P(PSTR("M600")); //save print and run M600 command
            }
            mmu_state = 1;
            if (mmu_cmd == 0)
                mmu_ready = true;
        }
        else if ((mmu_last_request + MMU_P0_TIMEOUT) < millis())
        {   //resend request after timeout (30s)
            mmu_state = 1;
        }
        return;
    case 3: //response to mmu commands
        if (mmu_rx_ok() > 0)
        {
            if (mmuFSensorLoading == false) {
                delay(100);
                printf_P(PSTR("MMU => 'ok'\n"));
                mmu_ready = true;
                mmu_state = 1;
            } else if (mmuFilamentMK3Moving == true) {
                mmu_printf_P(PSTR("FS%d\n"), 1);
                printf_P(PSTR("MMU <= 'Filament seen at extruder'\n"));
                mmuFSensorLoading = false;
                singleLog = true;
            } else {
                if (singleLog) {
                    printf_P(PSTR("MMU => 'waiting for filament @ MK3 Sensor'\n"));
                    singleLog = false;
                }
                mmu_printf_P(PSTR("FS%d\n"), 0);
            }
        }
        else if ((mmu_last_request + MMU_CMD_TIMEOUT) < millis())
        { //resend request after timeout (5 min)
        	printf_P(PSTR("MMU => 'Error State, do something here??'\n"));
            mmu_ready = false;
            mmu_state = 20;
        }
        return;
    case 10: //echo response, comms confirmation
        if (mmu_rx_echo() > 0)
        {
            printf_P(PSTR("MMU => 'CMD ACK'\n"));
            //mmu_puts_P(PSTR("EE\n")); // Advise MMU CMD is correct, execute
            ack_received = true;
            mmu_state = 1;            // Do normal Await command completion confirmation
        } else if ((mmu_last_request + 250) < millis()) {  // Timeout if echo doesn't match request, resend cmd
            mmu_state = 1;
        }
        return;
    case 20: //
        if (mmu_rx_ok() > 0)
        {
            //if ok received then go back to ready
            mmu_state = 1;
            mmu_ready = true;
        }
    }
}


void mmu_reset(void)
{
#ifdef MMU_HWRESET                             //HW - pulse reset pin
    digitalWrite(MMU_RST_PIN, LOW);
    _delay_us(100);
    digitalWrite(MMU_RST_PIN, HIGH);
#else                                          //SW - send X0 command
    mmu_puts_P(PSTR("X0\n"));
#endif
}

int8_t mmu_set_filament_type(uint8_t extruder, uint8_t filament)
{
    printf_P(PSTR("MMU <= 'F%d %d'\n"), extruder, filament);
    mmu_printf_P(PSTR("F%d %d\n"), extruder, filament);
    unsigned char timeout = MMU_TIMEOUT;       //10x100ms
    while ((mmu_rx_ok() <= 0) && (--timeout))
        delay_keep_alive(MMU_TODELAY);
    return timeout?1:0;
}

void mmu_command(uint8_t cmd)
{
    mmu_cmd = cmd;
    mmu_ready = false;
}

bool mmu_get_response(void)
{
//	printf_P(PSTR("mmu_get_response - begin\n"));
    KEEPALIVE_STATE(IN_PROCESS);
    while (mmu_cmd != 0)
    {
        delay_keep_alive(100);
    }
    while (!mmu_ready)
    {
        //if ((mmu_last_response + MMU_CMD_TIMEOUT) < millis()) break;
        if ((mmu_state != 3) || (mmu_state != 10)) break;
        delay_keep_alive(100);
    }
    bool ret = mmu_ready;
    mmu_ready = false;
//	printf_P(PSTR("mmu_get_response - end %d\n"), ret?1:0);
    return ret;
}

void manage_response(bool move_axes, bool turn_off_nozzle)
{
    bool response = false;
    mmu_print_saved = false;
    bool lcd_update_was_enabled = false;
    float hotend_temp_bckp = degTargetHotend(active_extruder);
    float z_position_bckp = current_position[Z_AXIS];
    float x_position_bckp = current_position[X_AXIS];
    float y_position_bckp = current_position[Y_AXIS];
    while(!response)
    {
        response = mmu_get_response(); //wait for "ok" from mmu
        if (!response) { //no "ok" or "echo" was received in reserved time frame, user will fix the issue on mmu unit
            if (!mmu_print_saved) { //first occurence, we are saving current position, park print head in certain position and disable nozzle heater
                if (lcd_update_enabled) {
                    lcd_update_was_enabled = true;
                    lcd_update_enable(false);
                }
                st_synchronize();
                mmu_print_saved = true;
                printf_P(PSTR("MMU not responding\n"));
                hotend_temp_bckp = degTargetHotend(active_extruder);
                if (move_axes) {
                    z_position_bckp = current_position[Z_AXIS];
                    x_position_bckp = current_position[X_AXIS];
                    y_position_bckp = current_position[Y_AXIS];

                    //lift z
                    current_position[Z_AXIS] += Z_PAUSE_LIFT;
                    if (current_position[Z_AXIS] > Z_MAX_POS) current_position[Z_AXIS] = Z_MAX_POS;
                    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 15, active_extruder);
                    st_synchronize();

                    //Move XY to side
                    current_position[X_AXIS] = X_PAUSE_POS;
                    current_position[Y_AXIS] = Y_PAUSE_POS;
                    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 50, active_extruder);
                    st_synchronize();
                }
                if (turn_off_nozzle) {
                    //set nozzle target temperature to 0
                    setAllTargetHotends(0);
                }
            }
            lcd_display_message_fullscreen_P(_i("MMU needs user attention. Fix the issue and then press button on MMU unit."));
            delay_keep_alive(1000);
        }
        else if (mmu_print_saved) {
            printf_P(PSTR("MMU starts responding\n"));
            if (turn_off_nozzle)
            {
                lcd_clear();
                setTargetHotend(hotend_temp_bckp, active_extruder);
                lcd_display_message_fullscreen_P(_i("MMU OK. Resuming temperature..."));
                delay_keep_alive(3000);
                while ((degTargetHotend(active_extruder) - degHotend(active_extruder)) > 5)
                {
                    delay_keep_alive(1000);
                    lcd_wait_for_heater();
                }
            }
            if (move_axes) {
                lcd_clear();
                lcd_display_message_fullscreen_P(_i("MMU OK. Resuming position..."));
                current_position[X_AXIS] = x_position_bckp;
                current_position[Y_AXIS] = y_position_bckp;
                plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 50, active_extruder);
                st_synchronize();
                current_position[Z_AXIS] = z_position_bckp;
                plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 15, active_extruder);
                st_synchronize();
            }
            else {
                lcd_clear();
                lcd_display_message_fullscreen_P(_i("MMU OK. Resuming..."));
                delay_keep_alive(1000); //delay just for showing MMU OK message for a while in case that there are no xyz movements
            }
        }
    }
    if (lcd_update_was_enabled) lcd_update_enable(true);
}

//! @brief load filament to nozzle of multimaterial printer
//!
//! This function is used only only after T? (user select filament) and M600 (change filament).
//! It is not used after T0 .. T4 command (select filament), in such case, gcode is responsible for loading
//! filament to nozzle.
//!
void mmu_load_to_nozzle()
{
    st_synchronize();

    bool saved_e_relative_mode = axis_relative_modes[E_AXIS];
    if (!saved_e_relative_mode) axis_relative_modes[E_AXIS] = true;
    current_position[E_AXIS] += 7.2f;
    float feedrate = 562;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], feedrate / 60, active_extruder);
    st_synchronize();
    current_position[E_AXIS] += 14.4f;
    feedrate = 871;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], feedrate / 60, active_extruder);
    st_synchronize();
    current_position[E_AXIS] += 36.0f;
    feedrate = 1393;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], feedrate / 60, active_extruder);
    st_synchronize();
    current_position[E_AXIS] += 14.4f;
    feedrate = 871;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], feedrate / 60, active_extruder);
    st_synchronize();
    if (!saved_e_relative_mode) axis_relative_modes[E_AXIS] = false;
}

void mmu_M600_wait_and_beep() {
    //Beep and wait for user to remove old filament and prepare new filament for load

    KEEPALIVE_STATE(PAUSED_FOR_USER);

    int counterBeep = 0;
    lcd_display_message_fullscreen_P(_i("Remove old filament and press the knob to start loading new filament."));
    bool bFirst=true;

    while (!lcd_clicked()) {
        manage_heater();
        manage_inactivity(true);

#if BEEPER > 0
        if (counterBeep == 500) {
            counterBeep = 0;
        }
        SET_OUTPUT(BEEPER);
        if (counterBeep == 0) {
            if((eSoundMode==e_SOUND_MODE_LOUD)||((eSoundMode==e_SOUND_MODE_ONCE)&&bFirst))
            {
                bFirst=false;
                WRITE(BEEPER, HIGH);
            }
        }
        if (counterBeep == 20) {
            WRITE(BEEPER, LOW);
        }

        counterBeep++;
#endif //BEEPER > 0

        delay_keep_alive(4);
    }
    WRITE(BEEPER, LOW);
}

void mmu_M600_load_filament(bool automatic)
{
    //load filament for mmu v2
    tmp_extruder = mmu_extruder;
    if (!automatic) {
#ifdef MMU_M600_SWITCH_EXTRUDER
        bool yes = lcd_show_fullscreen_message_yes_no_and_wait_P(_i("Do you want to switch extruder?"), false);
        if(yes) tmp_extruder = choose_extruder_menu();
#endif //MMU_M600_SWITCH_EXTRUDER
    }
    else {
        tmp_extruder = (tmp_extruder+1)%5;
    }
    lcd_update_enable(false);
    lcd_clear();
    lcd_set_cursor(0, 1);
    lcd_puts_P(_T(MSG_LOADING_FILAMENT));
    lcd_print(" ");
    lcd_print(tmp_extruder + 1);
    snmm_filaments_used |= (1 << tmp_extruder); //for stop print

//		  printf_P(PSTR("T code: %d \n"), tmp_extruder);
//		  mmu_printf_P(PSTR("T%d\n"), tmp_extruder);
    mmu_command(MMU_CMD_T0 + tmp_extruder);

    manage_response(false, true);
    mmu_command(MMU_CMD_C0);
    mmu_extruder = tmp_extruder; //filament change is finished

    mmu_load_to_nozzle();


    st_synchronize();
    current_position[E_AXIS]+= FILAMENTCHANGE_FINALFEED ;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2, active_extruder);
}


void extr_mov(float shift, float feed_rate)
{   //move extruder no matter what the current heater temperature is
    set_extrude_min_temp(.0);
    current_position[E_AXIS] += shift;
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], feed_rate, active_extruder);
    set_extrude_min_temp(EXTRUDE_MINTEMP);
}


void change_extr(int
#ifdef SNMM
                 extr
#endif //SNMM
                ) { //switches multiplexer for extruders
#ifdef SNMM
    st_synchronize();
    delay(100);

    disable_e0();
    disable_e1();
    disable_e2();

    mmu_extruder = extr;

    pinMode(E_MUX0_PIN, OUTPUT);
    pinMode(E_MUX1_PIN, OUTPUT);

    switch (extr) {
    case 1:
        WRITE(E_MUX0_PIN, HIGH);
        WRITE(E_MUX1_PIN, LOW);

        break;
    case 2:
        WRITE(E_MUX0_PIN, LOW);
        WRITE(E_MUX1_PIN, HIGH);

        break;
    case 3:
        WRITE(E_MUX0_PIN, HIGH);
        WRITE(E_MUX1_PIN, HIGH);

        break;
    default:
        WRITE(E_MUX0_PIN, LOW);
        WRITE(E_MUX1_PIN, LOW);

        break;
    }
    delay(100);
#endif
}

int get_ext_nr()
{   //reads multiplexer input pins and return current extruder number (counted from 0)
#ifndef SNMM
    return(mmu_extruder); //update needed
#else
    return(2 * READ(E_MUX1_PIN) + READ(E_MUX0_PIN));
#endif
}


void display_loading()
{
    switch (mmu_extruder)
    {
    case 1:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T1));
        break;
    case 2:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T2));
        break;
    case 3:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T3));
        break;
    default:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T0));
        break;
    }
}

void extr_adj(int extruder) //loading filament for SNMM
{
#ifndef SNMM
    uint8_t cmd = MMU_CMD_L0 + extruder;
    if (cmd > MMU_CMD_L4)
    {
        printf_P(PSTR("Filament out of range %d \n"),extruder);
        return;
    }
    mmu_command(cmd);

    //show which filament is currently loaded

    lcd_update_enable(false);
    lcd_clear();
    lcd_set_cursor(0, 1);
    lcd_puts_P(_T(MSG_LOADING_FILAMENT));
    //if(strlen(_T(MSG_LOADING_FILAMENT))>18) lcd.setCursor(0, 1);
    //else lcd.print(" ");
    lcd_print(" ");
    lcd_print(extruder + 1);

    // get response
    manage_response(false, false);

    lcd_update_enable(true);


    //lcd_return_to_status();
#else

    bool correct;
    max_feedrate[E_AXIS] =80;
    //max_feedrate[E_AXIS] = 50;
START:
    lcd_clear();
    lcd_set_cursor(0, 0);
    switch (extruder) {
    case 1:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T1));
        break;
    case 2:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T2));
        break;
    case 3:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T3));
        break;
    default:
        lcd_display_message_fullscreen_P(_T(MSG_FILAMENT_LOADING_T0));
        break;
    }
    KEEPALIVE_STATE(PAUSED_FOR_USER);
    do {
        extr_mov(0.001,1000);
        delay_keep_alive(2);
    } while (!lcd_clicked());
    //delay_keep_alive(500);
    KEEPALIVE_STATE(IN_HANDLER);
    st_synchronize();
    //correct = lcd_show_fullscreen_message_yes_no_and_wait_P(MSG_FIL_LOADED_CHECK, false);
    //if (!correct) goto	START;
    //extr_mov(BOWDEN_LENGTH/2.f, 500); //dividing by 2 is there because of max. extrusion length limitation (x_max + y_max)
    //extr_mov(BOWDEN_LENGTH/2.f, 500);
    extr_mov(bowden_length[extruder], 500);
    lcd_clear();
    lcd_set_cursor(0, 0);
    lcd_puts_P(_T(MSG_LOADING_FILAMENT));
    if(strlen(_T(MSG_LOADING_FILAMENT))>18) lcd_set_cursor(0, 1);
    else lcd_print(" ");
    lcd_print(mmu_extruder + 1);
    lcd_set_cursor(0, 2);
    lcd_puts_P(_T(MSG_PLEASE_WAIT));
    st_synchronize();
    max_feedrate[E_AXIS] = 50;
    lcd_update_enable(true);
    lcd_return_to_status();
    lcdDrawUpdate = 2;
#endif
}

struct E_step
{
    float extrude;   //!< extrude distance in mm
    float feed_rate; //!< feed rate in mm/s
};
static const E_step ramming_sequence[] PROGMEM =
{
    {1.0,   1000.0/60},
    {1.0,   1500.0/60},
    {2.0,   2000.0/60},
    {1.5,   3000.0/60},
    {2.5,   4000.0/60},
    {-15.0, 5000.0/60},
    {-14.0, 1200.0/60},
    {-6.0,  600.0/60},
    {10.0,  700.0/60},
    {-10.0, 400.0/60},
    {-50.0, 2000.0/60},
};

//! @brief Unload sequence to optimize shape of the tip of the unloaded filament
static void filament_ramming()
{
    for(uint8_t i = 0; i < (sizeof(ramming_sequence)/sizeof(E_step)); ++i)
    {
        current_position[E_AXIS] += pgm_read_float(&(ramming_sequence[i].extrude));
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS],
                         current_position[E_AXIS], pgm_read_float(&(ramming_sequence[i].feed_rate)), active_extruder);
        st_synchronize();
    }
}

void extr_unload()
{   //unload just current filament for multimaterial printers
#ifdef SNMM
    float tmp_motor[3] = DEFAULT_PWM_MOTOR_CURRENT;
    float tmp_motor_loud[3] = DEFAULT_PWM_MOTOR_CURRENT_LOUD;
    uint8_t SilentMode = eeprom_read_byte((uint8_t*)EEPROM_SILENT);
#endif

    if (degHotend0() > EXTRUDE_MINTEMP)
    {
#ifndef SNMM
        st_synchronize();

        //show which filament is currently unloaded
        lcd_update_enable(false);
        lcd_clear();
        lcd_set_cursor(0, 1);
        lcd_puts_P(_T(MSG_UNLOADING_FILAMENT));
        lcd_print(" ");
        lcd_print(mmu_extruder + 1);

        filament_ramming();

        mmu_command(MMU_CMD_U0);
        // get response
        manage_response(false, true);

        lcd_update_enable(true);
#else //SNMM

        lcd_clear();
        lcd_display_message_fullscreen_P(PSTR(""));
        max_feedrate[E_AXIS] = 50;
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_UNLOADING_FILAMENT));
        lcd_print(" ");
        lcd_print(mmu_extruder + 1);
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PLEASE_WAIT));
        if (current_position[Z_AXIS] < 15) {
            current_position[Z_AXIS] += 15; //lifting in Z direction to make space for extrusion
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 25, active_extruder);
        }

        current_position[E_AXIS] += 10; //extrusion
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 10, active_extruder);
        st_current_set(2, E_MOTOR_HIGH_CURRENT);
        if (current_temperature[0] < 230) { //PLA & all other filaments
            current_position[E_AXIS] += 5.4;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2800 / 60, active_extruder);
            current_position[E_AXIS] += 3.2;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3000 / 60, active_extruder);
            current_position[E_AXIS] += 3;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3400 / 60, active_extruder);
        }
        else { //ABS
            current_position[E_AXIS] += 3.1;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2000 / 60, active_extruder);
            current_position[E_AXIS] += 3.1;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2500 / 60, active_extruder);
            current_position[E_AXIS] += 4;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3000 / 60, active_extruder);
            /*current_position[X_AXIS] += 23; //delay
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 600 / 60, active_extruder); //delay
            current_position[X_AXIS] -= 23; //delay
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 600 / 60, active_extruder); //delay*/
            delay_keep_alive(4700);
        }

        max_feedrate[E_AXIS] = 80;
        current_position[E_AXIS] -= (bowden_length[mmu_extruder] + 60 + FIL_LOAD_LENGTH) / 2;
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 500, active_extruder);
        current_position[E_AXIS] -= (bowden_length[mmu_extruder] + 60 + FIL_LOAD_LENGTH) / 2;
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 500, active_extruder);
        st_synchronize();
        //st_current_init();
        if (SilentMode != SILENT_MODE_OFF) st_current_set(2, tmp_motor[2]); //set back to normal operation currents
        else st_current_set(2, tmp_motor_loud[2]);
        lcd_update_enable(true);
        lcd_return_to_status();
        max_feedrate[E_AXIS] = 50;
#endif //SNMM
    }
    else
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_ERROR));
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PREHEAT_NOZZLE));
        delay(2000);
        lcd_clear();
    }
    //lcd_return_to_status();
}

void extr_unload_at_boot()
{   //unload just current filament for multimaterial printers
#ifdef SNMM
    float tmp_motor[3] = DEFAULT_PWM_MOTOR_CURRENT;
    float tmp_motor_loud[3] = DEFAULT_PWM_MOTOR_CURRENT_LOUD;
    uint8_t SilentMode = eeprom_read_byte((uint8_t*)EEPROM_SILENT);
#endif

    if (degHotend0() > EXTRUDE_MINTEMP)
    {
#ifndef SNMM
        st_synchronize();

        //show which filament is currently unloaded
        lcd_update_enable(false);
        lcd_clear();
        lcd_set_cursor(0, 1);
        lcd_puts_P(_T(MSG_UNLOADING_FILAMENT));
        lcd_print(" ");
        lcd_print(mmu_extruder + 1);

        filament_ramming();

        //mmu_command(MMU_CMD_U0);
        // get response
        manage_response(false, true);

        lcd_update_enable(true);
#else //SNMM

        lcd_clear();
        lcd_display_message_fullscreen_P(PSTR(""));
        max_feedrate[E_AXIS] = 50;
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_UNLOADING_FILAMENT));
        lcd_print(" ");
        lcd_print(mmu_extruder + 1);
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PLEASE_WAIT));
        if (current_position[Z_AXIS] < 15) {
            current_position[Z_AXIS] += 15; //lifting in Z direction to make space for extrusion
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 25, active_extruder);
        }

        current_position[E_AXIS] += 10; //extrusion
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 10, active_extruder);
        st_current_set(2, E_MOTOR_HIGH_CURRENT);
        if (current_temperature[0] < 230) { //PLA & all other filaments
            current_position[E_AXIS] += 5.4;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2800 / 60, active_extruder);
            current_position[E_AXIS] += 3.2;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3000 / 60, active_extruder);
            current_position[E_AXIS] += 3;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3400 / 60, active_extruder);
        }
        else { //ABS
            current_position[E_AXIS] += 3.1;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2000 / 60, active_extruder);
            current_position[E_AXIS] += 3.1;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2500 / 60, active_extruder);
            current_position[E_AXIS] += 4;
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 3000 / 60, active_extruder);
            /*current_position[X_AXIS] += 23; //delay
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 600 / 60, active_extruder); //delay
            current_position[X_AXIS] -= 23; //delay
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 600 / 60, active_extruder); //delay*/
            delay_keep_alive(4700);
        }

        max_feedrate[E_AXIS] = 80;
        current_position[E_AXIS] -= (bowden_length[mmu_extruder] + 60 + FIL_LOAD_LENGTH) / 2;
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 500, active_extruder);
        current_position[E_AXIS] -= (bowden_length[mmu_extruder] + 60 + FIL_LOAD_LENGTH) / 2;
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 500, active_extruder);
        st_synchronize();
        //st_current_init();
        if (SilentMode != SILENT_MODE_OFF) st_current_set(2, tmp_motor[2]); //set back to normal operation currents
        else st_current_set(2, tmp_motor_loud[2]);
        lcd_update_enable(true);
        lcd_return_to_status();
        max_feedrate[E_AXIS] = 50;
#endif //SNMM
    }
    else
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_ERROR));
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PREHEAT_NOZZLE));
        delay(2000);
        lcd_clear();
    }
    //lcd_return_to_status();
}

//wrapper functions for loading filament
void extr_adj_0()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E0"));
#else
    change_extr(0);
    extr_adj(0);
#endif
}

void extr_adj_1()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E1"));
#else
    change_extr(1);
    extr_adj(1);
#endif
}

void extr_adj_2()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E2"));
#else
    change_extr(2);
    extr_adj(2);
#endif
}

void extr_adj_3()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E3"));
#else
    change_extr(3);
    extr_adj(3);
#endif
}

void extr_adj_4()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E4"));
#else
    change_extr(4);
    extr_adj(4);
#endif
}

void mmu_eject_fil_0()
{
    mmu_eject_filament(0, true);
}

void mmu_eject_fil_1()
{
    mmu_eject_filament(1, true);
}

void mmu_eject_fil_2()
{
    mmu_eject_filament(2, true);
}

void mmu_eject_fil_3()
{
    mmu_eject_filament(3, true);
}

void mmu_eject_fil_4()
{
    mmu_eject_filament(4, true);
}

void load_all()
{
#ifndef SNMM
    enquecommand_P(PSTR("M701 E0"));
    enquecommand_P(PSTR("M701 E1"));
    enquecommand_P(PSTR("M701 E2"));
    enquecommand_P(PSTR("M701 E3"));
    enquecommand_P(PSTR("M701 E4"));
#else
    for (int i = 0; i < 4; i++)
    {
        change_extr(i);
        extr_adj(i);
    }
#endif
}

//wrapper functions for changing extruders
void extr_change_0()
{
    change_extr(0);
    lcd_return_to_status();
}

void extr_change_1()
{
    change_extr(1);
    lcd_return_to_status();
}

void extr_change_2()
{
    change_extr(2);
    lcd_return_to_status();
}

void extr_change_3()
{
    change_extr(3);
    lcd_return_to_status();
}

//wrapper functions for unloading filament
void extr_unload_all()
{
    if (degHotend0() > EXTRUDE_MINTEMP)
    {
        for (int i = 0; i < 4; i++)
        {
            change_extr(i);
            extr_unload();
        }
    }
    else
    {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_ERROR));
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PREHEAT_NOZZLE));
        delay(2000);
        lcd_clear();
        lcd_return_to_status();
    }
}

//unloading just used filament (for snmm)
void extr_unload_used()
{
    if (degHotend0() > EXTRUDE_MINTEMP) {
        for (int i = 0; i < 4; i++) {
            if (snmm_filaments_used & (1 << i)) {
                change_extr(i);
                extr_unload();
            }
        }
        snmm_filaments_used = 0;
    }
    else {
        lcd_clear();
        lcd_set_cursor(0, 0);
        lcd_puts_P(_T(MSG_ERROR));
        lcd_set_cursor(0, 2);
        lcd_puts_P(_T(MSG_PREHEAT_NOZZLE));
        delay(2000);
        lcd_clear();
        lcd_return_to_status();
    }
}

void extr_unload_0()
{
    change_extr(0);
    extr_unload();
}

void extr_unload_1()
{
    change_extr(1);
    extr_unload();
}

void extr_unload_2()
{
    change_extr(2);
    extr_unload();
}

void extr_unload_3()
{
    change_extr(3);
    extr_unload();
}

void extr_unload_4()
{
    change_extr(4);
    extr_unload();
}

bool mmu_check_version()
{
    return (mmu_buildnr >= MMU_REQUIRED_FW_BUILDNR);
}

void mmu_show_warning()
{
    printf_P(PSTR("MMU2 firmware version invalid. Required version: build number %d or higher."), MMU_REQUIRED_FW_BUILDNR);
    kill(_i("Please update firmware in your MMU2. Waiting for reset."));
}

void mmu_eject_filament(uint8_t filament, bool recover)
{
    if (filament < 5)
    {

        if (degHotend0() > EXTRUDE_MINTEMP)
        {
            st_synchronize();

            {
                LcdUpdateDisabler disableLcdUpdate;
                lcd_clear();
                lcd_set_cursor(0, 1);
                lcd_puts_P(_i("Ejecting filament"));
                current_position[E_AXIS] -= 80;
                plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], 2500 / 60, active_extruder);
                st_synchronize();
                mmu_command(MMU_CMD_E0 + filament);
                manage_response(false, false);
                if (recover)
                {
                    lcd_show_fullscreen_message_and_wait_P(_i("Please remove filament and then press the knob."));
                    mmu_command(MMU_CMD_R0);
                    manage_response(false, false);
                }

            }
        }
        else
        {
            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_puts_P(_T(MSG_ERROR));
            lcd_set_cursor(0, 2);
            lcd_puts_P(_T(MSG_PREHEAT_NOZZLE));
            delay(2000);
            lcd_clear();
        }
    }
    else
    {
        puts_P(PSTR("Filament nr out of range!"));
    }
}
