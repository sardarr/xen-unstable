/*
 * xl_segment_proc.c
 * 
 * XenoLinux virtual disk proc interface .
 */

#include "xl_block.h"
#include <linux/proc_fs.h>
#include <linux/delay.h>

static struct proc_dir_entry *vhd;

extern unsigned short xldev_to_physdev(kdev_t xldev);

static int proc_read_vhd(char *page, char **start, off_t off,
			 int count, int *eof, void *data)
{
    return 0;
}

#define isdelim(c) \
  (c==' '||c==','||c=='\n'||c=='\r'||c=='\t'||c==':'||c=='('||c==')' ? 1 : 0)

char *get_string(char *string)                          /* a bit like strtok */
{
    static char *temp;
    int loop = 0;

    if (string != NULL)	
        temp = string;
    else
        string = temp;

 try_again:

    while (!isdelim(string[loop]))
    {
        if (string[loop] == '\0')
            return NULL;
        loop++;
    }

    string[loop] = '\0';	
    temp = (string + loop + 1);

    if (loop == 0)
    {
        string = temp;
        goto try_again;
    }

    return string;
}


#define isdigit(c) (c >= '0' && c <= '9' ? 1 : 0)
unsigned long to_number(char *string)                                /* atoi */
{
    unsigned long value = 0;

    if (string == NULL) return 0;

    while (!isdigit(*string) && *string != '\0') string++;

    while (isdigit(*string))
    {
        value = value * 10 + (*string - '0');
        string++;
    }

    return value;
}

static int proc_write_vhd(struct file *file, const char *buffer,
			  unsigned long count, void *data)
{
    char *local = kmalloc((count + 1) * sizeof(char), GFP_KERNEL);
    char *string;
    int loop;
    xv_disk_t xvd;
    int res;

    if (!local)
      return -ENOMEM;

    memset (&xvd, 0, sizeof(xvd));

    if (copy_from_user(local, buffer, count))
    {
	res = -EFAULT;
	goto out;
    }
    local[count] = '\0';

    res = count;
    string = get_string(local); /* domain specifier */
    if (string == NULL)
    {
	goto out;
    }
    if (*string != 'd' && *string != 'D')
    {
        printk (KERN_ALERT 
                "error: domain specifier missing [%s]. should be \"domain\".\n",
                string);
	goto out;
    }

    string = get_string(NULL); /* domain number */
    if (string == NULL)
    {
        printk (KERN_ALERT "error: domain number missing\n");
	goto out;
    }
    xvd.domain = (int) to_number(string);

    string = get_string(NULL);
    if (string && (strcmp(string, "RO") == 0 || strcmp(string, "ro") == 0))
    {
        xvd.mode = XEN_DISK_READ_ONLY;
    }
    else if (string && (strcmp(string, "RW") == 0 || strcmp(string, "rw") == 0))
    {
        xvd.mode = XEN_DISK_READ_WRITE;
    }
    else
    {
        printk (KERN_ALERT 
                "error: bad mode [%s]. should be \"rw\" or \"ro\".\n",
                string);
	goto out;
    }

    string = get_string(NULL);                           /* look for Segment */
    if (string == NULL || (*string != 's' && *string != 'S'))
    {
        printk (KERN_ALERT 
                "error: segment specifier missing [%s]. should be \"segment\".\n",
                string);
	goto out;
    }

    string = get_string(NULL);                             /* segment number */
    if (string == NULL)
    {
        printk (KERN_ALERT "error: segment number missing\n");
	goto out;
    }
    xvd.segment = (int) to_number(string);

    string = get_string(NULL);                           /* look for Extents */
    if (string == NULL || (*string != 'e' && *string != 'E'))
    {
        printk (KERN_ALERT 
                "error: extents specifier missing [%s]. should be \"extents\".\n",
                string);
	goto out;
    }

    string = get_string(NULL);                          /* number of extents */
    if (string == NULL)
    {
        printk (KERN_ALERT "error: number of extents missing\n");
	goto out;
    }
    xvd.ext_count = (int) to_number(string);

    /* ignore parenthesis */

    for (loop = 0; loop < xvd.ext_count; loop++)
    {
        string = get_string(NULL);                          /* look for Disk */
        if (string == NULL || (*string != 'd' && *string != 'D'))
        {
            printk (KERN_ALERT 
                    "hmm, extent disk specifier missing [%s]. should be \"disk\".\n",
                    string);
	    goto out;
        }
        string = get_string(NULL);                            /* disk number */
        if (string == NULL)
        {
            printk (KERN_ALERT "error: disk number missing\n");
	    goto out;
        }
        xvd.extents[loop].disk = xldev_to_physdev((int) to_number(string));

        string = get_string(NULL);                        /* look for Offset */
        if (string == NULL || (*string != 'o' && *string != 'O'))
        {
            printk (KERN_ALERT 
                    "error: disk offset missing [%s]. should be \"offset\".\n",
                    string);
	    goto out;
        }
        string = get_string(NULL);                                 /* offset */
        if (string == NULL)
        {
            printk (KERN_ALERT "error: offset missing\n");
	    goto out;
        }
        xvd.extents[loop].offset =  to_number(string);

        string = get_string(NULL);                          /* look for Size */
        if (string == NULL || (*string != 's' && *string != 'S'))
        {
            printk (KERN_ALERT 
                    "error: extent size missing [%s]. should be \"size\".\n",
                    string);
	    goto out;
        }
        string = get_string(NULL);                                   /* size */
        if (string == NULL)
        {
            printk (KERN_ALERT "error: extent size missing\n");
	    goto out;
        }
        xvd.extents[loop].size =  to_number(string);
    }

    xenolinux_control_msg(XEN_BLOCK_SEG_CREATE, (char *)&xvd, sizeof(xvd));

 out:
    kfree(local);

    return res;
}

/******************************************************************/

int __init xlseg_proc_init(void)
{
    vhd = create_proc_entry("xeno/dom0/vhd", 0644, NULL);
    if (vhd == NULL)
    {
        panic ("xlseg_init: unable to create vhd proc entry\n");
    }
    vhd->data       = NULL;
    vhd->read_proc  = proc_read_vhd;
    vhd->write_proc = proc_write_vhd;
    vhd->owner      = THIS_MODULE;

    printk(KERN_ALERT "XenoLinux Virtual Disk Device Monitor installed\n");
    return 0;
}

static void __exit xlseg_proc_cleanup(void)
{
    printk(KERN_ALERT "XenoLinux Virtual Disk Device Monitor uninstalled\n");
}

#ifdef MODULE
module_init(xlseg_proc_init);
module_exit(xlseg_proc_cleanup);
#endif
