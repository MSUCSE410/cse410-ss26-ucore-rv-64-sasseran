#include "file.h"
#include "defs.h"
#include "fcntl.h"
#include "fs.h"
#include "proc.h"

#define STAT_MODE_DIR  0x00400000
#define STAT_MODE_FILE 0x00100000

//This is a system-level open file table that holds open files of all process.
struct file filepool[FILEPOOLSIZE];

//Abstract the stdio into a file.
struct file *stdio_init(int fd)
{
	struct file *f = filealloc();
	f->type = FD_STDIO;
	f->ref = 1;
	f->readable = (fd == STDIN || fd == STDERR);
	f->writable = (fd == STDOUT || fd == STDERR);
	return f;
}

//The operation performed on the system-level open file table entry after some process closes a file.
void fileclose(struct file *f)
{
	if (f->ref < 1)
		panic("fileclose");
	if (--f->ref > 0) {
		return;
	}
	switch (f->type) {
	case FD_STDIO:
		// Do nothing
		break;
	case FD_INODE:
		iput(f->ip);
		break;
	default:
		panic("unknown file type %d\n", f->type);
	}

	f->off = 0;
	f->readable = 0;
	f->writable = 0;
	f->ref = 0;
	f->type = FD_NONE;
}

//Add a new system-level table entry for the open file table
struct file *filealloc()
{
	for (int i = 0; i < FILEPOOLSIZE; ++i) {
		if (filepool[i].ref == 0) {
			filepool[i].ref = 1;
			return &filepool[i];
		}
	}
	return 0;
}

//Show names of all files in the root_dir.
int show_all_files()
{
	return dirls(root_dir());
}

//Create a new empty file based on path and type and return its inode;
//if the file under the path exists, return its inode;
//returns 0 if the type of file to be created is not T_file
static struct inode *create(char *path, short type)
{
	struct inode *ip, *dp;
	dp = root_dir(); //Remember that the root_inode is open in this step,so it needs closing then.
	ivalid(dp);
	if ((ip = dirlookup(dp, path, 0)) != 0) {
		warnf("create a exist file\n");
		iput(dp); //Close the root_inode
		ivalid(ip);
		if (type == T_FILE && ip->type == T_FILE)
			return ip;
		iput(ip);
		return 0;
	}
	if ((ip = ialloc(dp->dev, type)) == 0)
		panic("create: ialloc");

	tracef("create dinode and inode type = %d\n", type);

	ivalid(ip);
	iupdate(ip);
	if (dirlink(dp, path, ip->inum) < 0)
		panic("create: dirlink");

	iput(dp);
	return ip;
}

//A process creates or opens a file according to its path, returning the file descriptor of the created or opened file.
//If omode is O_CREATE, create a new file
//if omode if the others,open a created file.
int fileopen(char *path, uint64 omode)
{
	int fd;
	struct file *f;
	struct inode *ip;
	if (omode & O_CREATE) {
		ip = create(path, T_FILE);
		if (ip == 0) {
			return -1;
		}
	} else {
		if ((ip = namei(path)) == 0) {
			return -1;
		}
		ivalid(ip);
	}
	if (ip->type != T_FILE)
		panic("unsupported file inode type\n");
	if ((f = filealloc()) == 0 ||
	    (fd = fdalloc(f)) <
		    0) { //Assign a system-level table entry to a newly created or opened file
		//and then create a file descriptor that points to it
		if (f)
			fileclose(f);
		iput(ip);
		return -1;
	}
	// only support FD_INODE
	f->type = FD_INODE;
	f->off = 0;
	f->ip = ip;
	f->readable = !(omode & O_WRONLY);
	f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
	if ((omode & O_TRUNC) && ip->type == T_FILE) {
		itrunc(ip);
	}
	return fd;
}

// Write data to inode.
uint64 inodewrite(struct file *f, uint64 va, uint64 len)
{
	int r;
	ivalid(f->ip);
	if ((r = writei(f->ip, 1, va, f->off, len)) > 0)
		f->off += r;
	return r;
}

//Read data from inode.
uint64 inoderead(struct file *f, uint64 va, uint64 len)
{
	int r;
	ivalid(f->ip);
	if ((r = readi(f->ip, 1, va, f->off, len)) > 0)
		f->off += r;
	return r;
}

// Copy from kernel to user.
int filestat(struct file *f, uint64 addr)
{
    if (f == 0 || f->type != FD_INODE || f->ip == 0) { // Validate the file pointer and ensure it's an inode file
        return -1;
    }

    struct proc *p = curr_proc(); // Get the current process to access its page table for copying data back to user space

	// Create a stat structure to hold the file information
    struct {
        uint64 dev;
        uint64 ino;
        uint32 mode;
        uint32 nlink;
        uint64 pad[7];
    } st;

	// Fill the stat structure with information from the file's inode
    st.dev = 0; // Device number is not used in this implementation, so we set it to 0
    st.ino = f->ip->inum; // Inode number from the file's inode
    st.mode = (f->ip->type == T_DIR) ? 0x00400000 : 0x00100000; // Set mode based on whether it's a directory or a regular file
    st.nlink = f->ip->nlink; // Number of links to the file from the inode's nlink field

	// Pad the remaining fields with zeros
    for (int i = 0; i < 7; i++) {
        st.pad[i] = 0;
    }

	// Copy the stat structure back to user space at the provided address
    if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0) {
        return -1;
    }

    return 0;
}

// Create a new link (also called hard link) to an existing file.
int filelink(char *old, char *new)
{
    struct inode *dp = root_dir(); // Get the root directory inode to perform lookups and modifications
    struct inode *ip = dirlookup(dp, old, 0); // Look up the inode for the existing file specified by 'old' path
    if (ip == 0) { // If the existing file does not exist, return an error
        iput(dp); // Release the reference to the root directory inode before returning
        return -1; // Return -1 to indicate failure in creating the link
    }

    ivalid(ip); // Ensure the inode for the existing file is valid and its data is loaded into memory

	// Create a new directory entry for the new link that points to the same inode as the existing file
    if (dirlink(dp, new, ip->inum) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink++; // Increment the link count of the inode since we have created a new link to it
    iupdate(ip); // Update the inode on disk to reflect the new link count

    iput(ip); // Release the reference to the existing file's inode
    iput(dp); // Release the reference to the root directory inode
    return 0;
}

// Remove a file from the file system.
int fileunlink(char *path)
{
    struct inode *dp = root_dir(); // Get the root directory inode to perform lookups and modifications
    struct inode *ip = dirlookup(dp, path, 0); // Look up the inode for the file specified by 'path' to be unlinked
    if (ip == 0) {
        iput(dp); // Release the reference to the root directory inode before returning if the file does not exist
        return -1; // Return -1 to indicate failure in unlinking the file since it does not exist
    }

    ivalid(ip); // Ensure the inode for the file to be unlinked is valid and its data is loaded into memory

	// Remove the directory entry for the file, effectively unlinking it from the file system
    if (dirunlink(dp, path) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink--; // Decrement the link count of the inode since we have removed one link to it
    iupdate(ip); // Update the inode on disk to reflect the new link count

    iput(ip);   // drop lookup reference
    iput(dp);   // drop directory reference

    return 0;
}