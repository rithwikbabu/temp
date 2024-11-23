//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "wmap.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}

int
sys_wmap(void) 
{
  uint addr;
  int length, flags, fd;
  struct file* file;
  struct proc* p = myproc();

  // fetch arguments
  if (argint(0, (int *)&addr) < 0 || argint(1, &length) < 0 || argint(2, &flags) < 0 || argint(3, &fd) < 0) {
    cprintf("sys_wmap: Unable to fetch arguments!\n");
    return FAILED;
  }

  // check if address is valid
  addr = PGROUNDDOWN(addr);
  if(length <= 0 || addr % PGSIZE != 0 || addr < 0x60000000 || addr >= 0x80000000) {
    cprintf("sys_wmap: Invalid address or length!\n");
    return FAILED;
  }
    
  // check flags
  if (!(flags & MAP_SHARED)) {
    cprintf("sys_wmap: MAP_SHARED flag not set!\n");
    return FAILED;
  }

  if (!(flags & MAP_FIXED)) {
    cprintf("sys_wmap: MAP_FIXED flag not set!\n");
    return FAILED;
  }

  if (!(flags & MAP_ANONYMOUS)) {
    // check file descriptor
    if (argfd(3, &fd, &file) < 0) {
      cprintf("sys_wmap: Unable to fetch file descriptor!\n");
      return FAILED;
    }
    if ((fd = fdalloc(file)) < 0) {
      cprintf("sys_wmap: Unable to allocate file descriptor.\n");
      return FAILED;
    }
    filedup(file);
  } else {
    fd = -1;
    file = 0;
  }

  // check if theres room for the mapping
  struct mapping* m = p->mappings;
  while (m) {
    if (addr < m->addr + m->length && m->addr < addr + length) {
      cprintf("sys_wmap: Address overlaps with existing mapping!\n");
      return FAILED;
    }
    m = m->next;
  }

  // allocate and initialize mapping
  struct mapping* map = (struct mapping*)kalloc();
  if (!map) {
    cprintf("sys_wmap: Unable to allocate memory for mapping record!\n");
    return FAILED;
  }
  map->addr = addr;
  map->length = length;
  map->flag = flags & MAP_ANONYMOUS;
  map->fd = fd;
  map->file = file;
  map->count = 0;

  // update linked list
  map->next = p->mappings;
  p->mappings = map;

  return addr;
}

int
sys_wunmap(void)
{
  uint addr;
  struct proc* p = myproc();

  // fetch arguments
  if (argint(0, (int *)&addr) < 0) {
    cprintf("sys_wunmap: Unable to fetch arguments!\n");
    return FAILED;
  }

  // check if address is valid
  if (addr < 0x60000000 || addr >= 0x80000000) {
    cprintf("sys_wunmap: Address out of valid range!\n");
    return FAILED;
  }

  // locate the mapping to remove
  struct mapping* map = 0;
  struct mapping* m = p->mappings;

  while (m) {
      if (addr == m->addr) {
          map = m;
          break;
      }
      m = m->next;
  }

  // check if mapping was found
  if (!map) {
      cprintf("sys_wunmap: Mapping not found for address!\n");
      return FAILED;
  }

  // calc the number of pages
  int page_count = map->length / PGSIZE;
  if (map->length % PGSIZE > 0) {
    page_count++;
  }

  // unmap
  for (int i = 0; i < page_count; i++) {
    pte_t* pte = walkpgdir(p->pgdir, (char*)(addr + i * PGSIZE), 0);
    if (pte && (*pte & PTE_P)) {
      uint pa = PTE_ADDR(*pte);
      
      // if anonymous
      if (map->flag == 0 && map->file) {
          filewrite(map->file, P2V(pa), PGSIZE);
      }

      // decrement reference count and free memory if needed
      ref_cnts[pa / PGSIZE]--;
      if (pa >= EXTMEM && pa < PHYSTOP && ref_cnts[pa / PGSIZE] == 0) {
          kfree(P2V(pa));
      }
      *pte = 0;
    }
  }

  // remove mapping from mappings list
  struct mapping* prev = 0;
  m = p->mappings;
  while (m) {
    if (m->addr == addr) {
      if (prev) {
        prev->next = m->next;
      } else {
        p->mappings = m->next;
      }
      break;
    }
    prev = m;
    m = m->next;
  }

  return SUCCESS;
}

int
sys_va2pa(void)
{
  uint va;
  struct proc *p = myproc();

  // fetch argument
  if (argint(0, (int *)&va) < 0) {
    cprintf("sys_va2pa: Unable to fetch virtual address argument!\n");
    return FAILED;
  }

  // get page directory entry
  pde_t *pde = &(p->pgdir)[PDX(va)];
  if(*pde & PTE_P){
    // get page table entry
    pte_t *page_table = (pte_t*)P2V(PTE_ADDR(*pde));
    pte_t *pte = &page_table[PTX(va)];

    // append the offset
    if (*pte & PTE_P) {
      return PTE_ADDR(*pte) | PTE_FLAGS(va);
    }
  }

  return FAILED;
}

int 
sys_getwmapinfo(void) {
  struct wmapinfo* wmap;
  struct proc *p = myproc();

  // fetch arg
  if (argptr(0, (void *)&wmap, sizeof(struct wmapinfo)) < 0) {
    cprintf("sys_getwmapinfo: Unable to fetch argument!\n");
    return FAILED;
  }

  // construct wmapinfo
  struct wmapinfo* wminfo = (struct wmapinfo*) wmap;
  struct mapping* m = p->mappings;
  int i = 0;

  while (m && i < MAX_WMMAP_INFO) {
    wminfo->addr[i] = m->addr;
    wminfo->length[i] = m->length;
    wminfo->n_loaded_pages[i] = m->count;

    m = m->next;
    i++;
  }

  // set total mappings
  wminfo->total_mmaps = i;

  return SUCCESS;
}
