/*
 * $Id$
 *
 *  Copyright (c) 2001 "Crazy" James Simmons jsimmons@transvirtual.com 
 *
 *  Sponsored by SuSE, Transvirtual Technology. 
 * 
 *  Derived from the code in h3600_ts.[ch] by Charles Flynn  
 */

/*
 * Driver for the h3600 Touch Screen and other Atmel controlled devices.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by
 * e-mail - mail your message to <jsimmons@transvirtual.com>.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>

/* SA1100 serial defines */
#include <asm/arch/hardware.h>
#include <asm/arch/serial_reg.h>
#include <asm/arch/irqs.h>

/*
 * Definitions & global arrays.
 */

/* The start and end of frame characters SOF and EOF */
#define CHAR_SOF                0x02
#define CHAR_EOF                0x03
#define FRAME_OVERHEAD          3       /* CHAR_SOF,CHAR_EOF,LENGTH = 3 */

/*
        Atmel events and response IDs contained in frame.
        Programmer has no control over these numbers.
        TODO there are holes - specifically  1,7,0x0a
*/
#define VERSION_ID              0       /* Get Version (request/respose) */
#define KEYBD_ID                2       /* Keyboard (event) */
#define TOUCHS_ID               3       /* Touch Screen (event)*/
#define EEPROM_READ_ID          4       /* (request/response) */
#define EEPROM_WRITE_ID         5       /* (request/response) */
#define THERMAL_ID              6       /* (request/response) */
#define NOTIFY_LED_ID           8       /* (request/response) */
#define BATTERY_ID              9       /* (request/response) */
#define SPI_READ_ID             0x0b    /* ( request/response) */
#define SPI_WRITE_ID            0x0c    /* ( request/response) */
#define FLITE_ID                0x0d    /* backlight ( request/response) */
#define STX_ID                  0xa1    /* extension pack status (req/resp) */

#define MAX_ID                  14

#define H3600_MAX_LENGTH 16
#define H3600_KEY 0xf 

#define H3600_SCANCODE_RECORD	1	 /* 1 -> record button */
#define H3600_SCANCODE_CALENDAR 2	 /* 2 -> calendar */
#define H3600_SCANCODE_CONTACTS 3	 /* 3 -> contact */
#define H3600_SCANCODE_Q	4	 /* 4 -> Q button */
#define	H3600_SCANCODE_START	5	 /* 5 -> start menu */
#define	H3600_SCANCODE_UP	6	 /* 6 -> up */
#define H3600_SCANCODE_RIGHT	7 	 /* 7 -> right */
#define H3600_SCANCODE_LEFT 	8	 /* 8 -> left */
#define H3600_SCANCODE_DOWN 	9	 /* 9 -> down */

static char *h3600_name = "H3600 TouchScreen";

/*
 * Per-touchscreen data.
 */
struct h3600_dev {
	struct input_dev dev;
	struct serio *serio;
	unsigned char event;	/* event ID from packet */
	unsigned char chksum;
	unsigned char len;
	unsigned char idx;
	unsigned char buf[H3600_MAX_LENGTH];
};

/*
static int h3600_write(struct serio *dev, unsigned char val)
{
	return 0;
}

static struct serio h3600_port =
{
        type:   SERIO_RS232,
        write:  h3600_write,
};
*/

static void action_button_handler(int irq, void *dev_id, struct pt_regs *regs)
{
        int down = (GPLR & GPIO_BITSY_ACTION_BUTTON) ? 0 : 1;
	struct input_dev *dev = (struct input_dev *) dev_id;

	input_report_key(dev, KEY_ENTER, down);
}

/* ??? Why place it in a task queue */
static int suspend_button_pushed = 0;
static int suspend_button_mode = 1;

static void npower_button_handler(int irq, void *dev_id, struct pt_regs *regs)
{
        int down = (GPLR & GPIO_BITSY_NPOWER_BUTTON) ? 0 : 1;
	struct input_dev *dev = (struct input_dev *) dev_id;

	/* 
	 * This interrupt is only called when we release the key. So we have 
	 * to fake a key press.
	 */ 	
	input_report_key(dev, KEY_SUSPEND, 1);
	input_report_key(dev, KEY_SUSPEND, down); 	
/*
        if (suspend_button_mode) {
               	input_report_key(dev, KEY_SUSPEND, down); 	
        } else {
                if (!suspend_button_pushed) {
                        extern void pm_do_suspend(void);

			suspend_button_pushed = 1;
        
			udelay(200);  debounce 
        		pm_do_suspend();
        		suspend_button_pushed = 0;
                }
        }
*/
}

#ifdef CONFIG_PM
static int suspended = 0;
static int h3600_ts_pm_callback(struct pm_dev *pm_dev, pm_request_t req, 
				void *data)
{
        switch (req) {
        case PM_SUSPEND: /* enter D1-D3 */
                suspended = 1;
                //h3600_flite_power(FLITE_PWR_OFF);
                break;
        case PM_BLANK:
                if (!suspended)
                        //h3600_flite_power(FLITE_PWR_OFF);
                break;
        case PM_RESUME:  /* enter D0 */
                /* same as unblank */
        case PM_UNBLANK:
                if (suspended) {
                        //initSerial();
                        suspended = 0;
                }
               // h3600_flite_power(FLITE_PWR_ON);
                break;
        }
        return 0;
}
#endif

/*
 * This function translates the native event packets to linux input event
 * packets. Some packets coming from ldisc are not touchscreen related. In
 * this case we send them off to be processed elsewhere. 
 */
static void h3600ts_process_packet(struct h3600_dev *ts)
{
        struct input_dev *dev = &ts->dev;
	static int touched = 0;
	int key, down = 0;

        switch (ts->event) {
                /*
                   Buttons - returned as a single byte
                        7 6 5 4 3 2 1 0
                        S x x x N N N N

                   S       switch state ( 0=pressed 1=released)
                   x       Unused.
                   NNNN    switch number 0-15

                Note: This is true for non interrupt generated key events.
                */
                case KEYBD_ID:
			down = (ts->buf[0] & 0x80) ? 0 : 1; 

			switch (ts->buf[0] & 0x7f) {
				case H3600_SCANCODE_RECORD:
					key = KEY_RECORD;
					break;
				case H3600_SCANCODE_CALENDAR:
					key = 0;
                                        break;
				case H3600_SCANCODE_CONTACTS:
					key = 0;
                                        break;
				case H3600_SCANCODE_Q:        
					key = KEY_Q;
                                        break;
				case H3600_SCANCODE_START:
					key = 0;
                                        break;
				case H3600_SCANCODE_UP:
					key = KEY_UP;
                                        break;
				case H3600_SCANCODE_RIGHT:
					key = KEY_RIGHT;
                                        break;
				case H3600_SCANCODE_LEFT:
					key = KEY_LEFT;
                                        break;
				case H3600_SCANCODE_DOWN:
					key = KEY_DOWN;
                                        break;
				default:
					key = 0;	
			}	
                        if (key) 
                        	input_report_key(dev, key, down);
                        break;
                /*
                 * Native touchscreen event data is formatted as shown below:-
                 *
                 *      +-------+-------+-------+-------+
                 *      | Xmsb  | Xlsb  | Ymsb  | Ylsb  |
                 *      +-------+-------+-------+-------+
                 *       byte 0    1       2       3
                 */
                case TOUCHS_ID:
			if (!touched) { 
				input_report_key(dev, BTN_TOUCH, 1);
				touched = 1;
			}			

			if (ts->len) {
				unsigned short x, y;				

				x = ts->buf[0]; x <<= 8; x += ts->buf[1];
                                y = ts->buf[2]; y <<= 8; y += ts->buf[3];

                       		input_report_abs(dev, ABS_X, x);
                       		input_report_abs(dev, ABS_Y, y);
			} else {
		               	input_report_key(dev, BTN_TOUCH, 0);
				touched = 0;
			}	
                        break;
		default:
			/* Send a non input event elsewhere */
			break;
        }
}

/*
 * h3600ts_event() handles events from the input module.
 */
static int h3600ts_event(struct input_dev *dev, unsigned int type, 
		 	 unsigned int code, int value)
{
	struct h3600_dev *ts = dev->private;

	switch (type) {
		case EV_LED:
			//ts->serio->write(ts->serio, SOME_CMD);
			return 0;
		/* 
		 * We actually provide power management when you press the
		 * power management button= 
		 */
		case EV_KEY:
			if (code == KEY_SUSPEND) {
				printk("Handling power key\n");
				if (value == 0) {
					/* Turn off the power */
					//h3600_flite_power(FLITE_PWR_OFF);
				} else {
					/* Lite this little light of mine */
					//h3600_flite_power(FLITE_PWR_ON);
				}  		  
			}  	
			return 0;	
	}					
	return -1;
}

/*
        Frame format
  byte    1       2               3              len + 4
        +-------+---------------+---------------+--=------------+
        |SOF    |id     |len    | len bytes     | Chksum        |
        +-------+---------------+---------------+--=------------+
  bit   0     7  8    11 12   15 16

        +-------+---------------+-------+
        |SOF    |id     |0      |Chksum | - Note Chksum does not include SOF
        +-------+---------------+-------+
  bit   0     7  8    11 12   15 16

*/

static int state;

/* decode States  */
#define STATE_SOF       0       /* start of FRAME */
#define STATE_ID        1       /* state where we decode the ID & len */
#define STATE_DATA      2       /* state where we decode data */
#define STATE_EOF       3       /* state where we decode checksum or EOF */

static void h3600ts_interrupt(struct serio *serio, unsigned char data,
                              unsigned int flags)
{
        struct h3600_dev *ts = serio->private;

	/*
         * We have a new frame coming in. 
         */
	switch (state) {
		case STATE_SOF:
        		if (data == CHAR_SOF)
                		state = STATE_ID;	
			return;
        	case STATE_ID:
			ts->event = (data & 0xf0) >> 4;
			ts->len = (data & 0xf);
			ts->idx = 0;
			if (ts->event >= MAX_ID) {
				state = STATE_SOF;
                        	break;
			}
			ts->chksum = data;
                	state=(ts->len > 0 ) ? STATE_DATA : STATE_EOF;
			break;
		case STATE_DATA:
			ts->chksum += data;
			ts->buf[ts->idx]= data;
			if(++ts->idx == ts->len) 
                        	state = STATE_EOF;
			break;
		case STATE_EOF:
                	state = STATE_SOF;
                	if (data == CHAR_EOF || data == ts->chksum )
				h3600ts_process_packet(ts);
                	break;
        	default:
                	printk("Error3\n");
                	break;
	}
}

/*
 * h3600ts_connect() is the routine that is called when someone adds a
 * new serio device. It looks whether it was registered as a H3600 touchscreen
 * and if yes, registers it as an input device.
 */
static void h3600ts_connect(struct serio *serio, struct serio_dev *dev)
{
	struct h3600_dev *ts;

	if (serio->type != (SERIO_RS232 | SERIO_H3600))
		return;

	if (!(ts = kmalloc(sizeof(struct h3600_dev), GFP_KERNEL)))
		return;

	memset(ts, 0, sizeof(struct h3600_dev));

	/* Device specific stuff */
        set_GPIO_IRQ_edge( GPIO_BITSY_ACTION_BUTTON, GPIO_BOTH_EDGES );
        set_GPIO_IRQ_edge( GPIO_BITSY_NPOWER_BUTTON, GPIO_RISING_EDGE );

        if (request_irq(IRQ_GPIO_BITSY_ACTION_BUTTON, action_button_handler,
			SA_SHIRQ | SA_INTERRUPT | SA_SAMPLE_RANDOM,	
			"h3600_action", &ts->dev)) {
		printk(KERN_ERR "h3600ts.c: Could not allocate Action Button IRQ!\n");
		kfree(ts);
		return;
	}

        if (request_irq(IRQ_GPIO_BITSY_NPOWER_BUTTON, npower_button_handler,
			SA_SHIRQ | SA_INTERRUPT | SA_SAMPLE_RANDOM, 
			"h3600_suspend", &ts->dev)) {
		free_irq(IRQ_GPIO_BITSY_ACTION_BUTTON, &ts->dev);
		printk(KERN_ERR "h3600ts.c: Could not allocate Power Button IRQ!\n");
		kfree(ts);
		return;
	}
	/* Now we have things going we setup our input device */
	ts->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_ABS) | BIT(EV_LED);	
	ts->dev.absbit[0] = BIT(ABS_X) | BIT(ABS_Y);
	ts->dev.ledbit[0] = BIT(LED_SLEEP); 

	/* values ???? */
	ts->dev.absmin[ABS_X] = 0;   ts->dev.absmin[ABS_Y] = 0;
	ts->dev.absmax[ABS_X] = 100; ts->dev.absmax[ABS_Y] = 100;
	//ts->dev.absfuzz[ABS_X] = 1; ts->dev.absfuzz[ABS_Y] = 1;

	ts->serio = serio;
	serio->private = ts;

	set_bit(KEY_RECORD, ts->dev.keybit);
	set_bit(KEY_Q, ts->dev.keybit);
	set_bit(KEY_UP, ts->dev.keybit);
	set_bit(KEY_RIGHT, ts->dev.keybit);
	set_bit(KEY_LEFT, ts->dev.keybit);
	set_bit(KEY_DOWN, ts->dev.keybit);
	set_bit(KEY_ENTER, ts->dev.keybit);
	ts->dev.keybit[LONG(BTN_TOUCH)] |= BIT(BTN_TOUCH);
	ts->dev.keybit[LONG(KEY_SUSPEND)] |= BIT(KEY_SUSPEND);

       	ts->dev.event = h3600ts_event;
	ts->dev.private = ts;
	ts->dev.name = h3600_name;
	ts->dev.idbus = BUS_RS232;
	ts->dev.idvendor = SERIO_H3600;
	ts->dev.idproduct = 0x0666;  /* FIXME !!! We can ask the hardware */
	ts->dev.idversion = 0x0100;

	if (serio_open(serio, dev)) {
        	free_irq(IRQ_GPIO_BITSY_ACTION_BUTTON, ts);
        	free_irq(IRQ_GPIO_BITSY_NPOWER_BUTTON, ts);
		kfree(ts);
		return;
	}

	//h3600_flite_control(1, 25);     /* default brightness */
#ifdef CONFIG_PM
	pm_register(PM_ILLUMINATION_DEV, PM_SYS_LIGHT, h3600_ts_pm_callback);
	printk("registered pm callback\n");
#endif
	input_register_device(&ts->dev);

	printk(KERN_INFO "input%d: %s on serio%d\n", ts->dev.number, 
		h3600_name, serio->number);
}

/*
 * h3600ts_disconnect() is the opposite of h3600ts_connect()
 */

static void h3600ts_disconnect(struct serio *serio)
{
	struct h3600_dev *ts = serio->private;
	
        free_irq(IRQ_GPIO_BITSY_ACTION_BUTTON, &ts->dev);
        free_irq(IRQ_GPIO_BITSY_NPOWER_BUTTON, &ts->dev);
#ifdef CONFIG_PM
        pm_unregister_all(h3600_ts_pm_callback);
#endif
	input_unregister_device(&ts->dev);
	serio_close(serio);
	kfree(ts);
}

/*
 * The serio device structure.
 */

static struct serio_dev h3600ts_dev = {
	interrupt:	h3600ts_interrupt,
	connect:	h3600ts_connect,
	disconnect:	h3600ts_disconnect,
};

/*
 * The functions for inserting/removing us as a module.
 */

static int __init h3600ts_init(void)
{
	serio_register_device(&h3600ts_dev);
	return 0;
}

static void __exit h3600ts_exit(void)
{
	serio_unregister_device(&h3600ts_dev);
}

module_init(h3600ts_init);
module_exit(h3600ts_exit);
