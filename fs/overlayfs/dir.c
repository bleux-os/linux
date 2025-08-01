// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2011 Novell Inc.
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/module.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <linux/backing-file.h>
#include "overlayfs.h"

static unsigned short ovl_redirect_max = 256;
module_param_named(redirect_max, ovl_redirect_max, ushort, 0644);
MODULE_PARM_DESC(redirect_max,
		 "Maximum length of absolute redirect xattr value");

static int ovl_set_redirect(struct dentry *dentry, bool samedir);

static int ovl_cleanup_locked(struct ovl_fs *ofs, struct inode *wdir,
			      struct dentry *wdentry)
{
	int err;

	dget(wdentry);
	if (d_is_dir(wdentry))
		err = ovl_do_rmdir(ofs, wdir, wdentry);
	else
		err = ovl_do_unlink(ofs, wdir, wdentry);
	dput(wdentry);

	if (err) {
		pr_err("cleanup of '%pd2' failed (%i)\n",
		       wdentry, err);
	}

	return err;
}

int ovl_cleanup(struct ovl_fs *ofs, struct dentry *workdir,
		struct dentry *wdentry)
{
	int err;

	err = ovl_parent_lock(workdir, wdentry);
	if (err)
		return err;

	ovl_cleanup_locked(ofs, workdir->d_inode, wdentry);
	ovl_parent_unlock(workdir);

	return 0;
}

struct dentry *ovl_lookup_temp(struct ovl_fs *ofs, struct dentry *workdir)
{
	struct dentry *temp;
	char name[20];
	static atomic_t temp_id = ATOMIC_INIT(0);

	/* counter is allowed to wrap, since temp dentries are ephemeral */
	snprintf(name, sizeof(name), "#%x", atomic_inc_return(&temp_id));

	temp = ovl_lookup_upper(ofs, name, workdir, strlen(name));
	if (!IS_ERR(temp) && temp->d_inode) {
		pr_err("workdir/%s already exists\n", name);
		dput(temp);
		temp = ERR_PTR(-EIO);
	}

	return temp;
}

static struct dentry *ovl_whiteout(struct ovl_fs *ofs)
{
	int err;
	struct dentry *whiteout;
	struct dentry *workdir = ofs->workdir;
	struct inode *wdir = workdir->d_inode;

	guard(mutex)(&ofs->whiteout_lock);

	if (!ofs->whiteout) {
		inode_lock_nested(wdir, I_MUTEX_PARENT);
		whiteout = ovl_lookup_temp(ofs, workdir);
		if (!IS_ERR(whiteout)) {
			err = ovl_do_whiteout(ofs, wdir, whiteout);
			if (err) {
				dput(whiteout);
				whiteout = ERR_PTR(err);
			}
		}
		inode_unlock(wdir);
		if (IS_ERR(whiteout))
			return whiteout;
		ofs->whiteout = whiteout;
	}

	if (!ofs->no_shared_whiteout) {
		inode_lock_nested(wdir, I_MUTEX_PARENT);
		whiteout = ovl_lookup_temp(ofs, workdir);
		if (!IS_ERR(whiteout)) {
			err = ovl_do_link(ofs, ofs->whiteout, wdir, whiteout);
			if (err) {
				dput(whiteout);
				whiteout = ERR_PTR(err);
			}
		}
		inode_unlock(wdir);
		if (!IS_ERR(whiteout))
			return whiteout;
		if (PTR_ERR(whiteout) != -EMLINK) {
			pr_warn("Failed to link whiteout - disabling whiteout inode sharing(nlink=%u, err=%lu)\n",
				ofs->whiteout->d_inode->i_nlink,
				PTR_ERR(whiteout));
			ofs->no_shared_whiteout = true;
		}
	}
	whiteout = ofs->whiteout;
	ofs->whiteout = NULL;
	return whiteout;
}

int ovl_cleanup_and_whiteout(struct ovl_fs *ofs, struct dentry *dir,
			     struct dentry *dentry)
{
	struct dentry *whiteout;
	int err;
	int flags = 0;

	whiteout = ovl_whiteout(ofs);
	err = PTR_ERR(whiteout);
	if (IS_ERR(whiteout))
		return err;

	if (d_is_dir(dentry))
		flags = RENAME_EXCHANGE;

	err = ovl_lock_rename_workdir(ofs->workdir, whiteout, dir, dentry);
	if (!err) {
		err = ovl_do_rename(ofs, ofs->workdir, whiteout, dir, dentry, flags);
		unlock_rename(ofs->workdir, dir);
	}
	if (err)
		goto kill_whiteout;
	if (flags)
		ovl_cleanup(ofs, ofs->workdir, dentry);

out:
	dput(whiteout);
	return err;

kill_whiteout:
	ovl_cleanup(ofs, ofs->workdir, whiteout);
	goto out;
}

struct dentry *ovl_create_real(struct ovl_fs *ofs, struct dentry *parent,
			       struct dentry *newdentry, struct ovl_cattr *attr)
{
	struct inode *dir = parent->d_inode;
	int err;

	if (IS_ERR(newdentry))
		return newdentry;

	err = -ESTALE;
	if (newdentry->d_inode)
		goto out;

	if (attr->hardlink) {
		err = ovl_do_link(ofs, attr->hardlink, dir, newdentry);
	} else {
		switch (attr->mode & S_IFMT) {
		case S_IFREG:
			err = ovl_do_create(ofs, dir, newdentry, attr->mode);
			break;

		case S_IFDIR:
			/* mkdir is special... */
			newdentry =  ovl_do_mkdir(ofs, dir, newdentry, attr->mode);
			err = PTR_ERR_OR_ZERO(newdentry);
			break;

		case S_IFCHR:
		case S_IFBLK:
		case S_IFIFO:
		case S_IFSOCK:
			err = ovl_do_mknod(ofs, dir, newdentry, attr->mode,
					   attr->rdev);
			break;

		case S_IFLNK:
			err = ovl_do_symlink(ofs, dir, newdentry, attr->link);
			break;

		default:
			err = -EPERM;
		}
	}
	if (!err && WARN_ON(!newdentry->d_inode)) {
		/*
		 * Not quite sure if non-instantiated dentry is legal or not.
		 * VFS doesn't seem to care so check and warn here.
		 */
		err = -EIO;
	}
out:
	if (err) {
		if (!IS_ERR(newdentry))
			dput(newdentry);
		return ERR_PTR(err);
	}
	return newdentry;
}

struct dentry *ovl_create_temp(struct ovl_fs *ofs, struct dentry *workdir,
			       struct ovl_cattr *attr)
{
	struct dentry *ret;
	inode_lock(workdir->d_inode);
	ret = ovl_create_real(ofs, workdir,
			      ovl_lookup_temp(ofs, workdir), attr);
	inode_unlock(workdir->d_inode);
	return ret;
}

static int ovl_set_opaque_xerr(struct dentry *dentry, struct dentry *upper,
			       int xerr)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	int err;

	err = ovl_check_setxattr(ofs, upper, OVL_XATTR_OPAQUE, "y", 1, xerr);
	if (!err)
		ovl_dentry_set_opaque(dentry);

	return err;
}

static int ovl_set_opaque(struct dentry *dentry, struct dentry *upperdentry)
{
	/*
	 * Fail with -EIO when trying to create opaque dir and upper doesn't
	 * support xattrs. ovl_rename() calls ovl_set_opaque_xerr(-EXDEV) to
	 * return a specific error for noxattr case.
	 */
	return ovl_set_opaque_xerr(dentry, upperdentry, -EIO);
}

/*
 * Common operations required to be done after creation of file on upper.
 * If @hardlink is false, then @inode is a pre-allocated inode, we may or
 * may not use to instantiate the new dentry.
 */
static int ovl_instantiate(struct dentry *dentry, struct inode *inode,
			   struct dentry *newdentry, bool hardlink, struct file *tmpfile)
{
	struct ovl_inode_params oip = {
		.upperdentry = newdentry,
		.newinode = inode,
	};

	ovl_dentry_set_upper_alias(dentry);
	ovl_dentry_init_reval(dentry, newdentry, NULL);

	if (!hardlink) {
		/*
		 * ovl_obtain_alias() can be called after ovl_create_real()
		 * and before we get here, so we may get an inode from cache
		 * with the same real upperdentry that is not the inode we
		 * pre-allocated.  In this case we will use the cached inode
		 * to instantiate the new dentry.
		 *
		 * XXX: if we ever use ovl_obtain_alias() to decode directory
		 * file handles, need to use ovl_get_inode_locked() and
		 * d_instantiate_new() here to prevent from creating two
		 * hashed directory inode aliases.  We then need to return
		 * the obtained alias to ovl_mkdir().
		 */
		inode = ovl_get_inode(dentry->d_sb, &oip);
		if (IS_ERR(inode))
			return PTR_ERR(inode);
		if (inode == oip.newinode)
			ovl_set_flag(OVL_UPPERDATA, inode);
	} else {
		WARN_ON(ovl_inode_real(inode) != d_inode(newdentry));
		dput(newdentry);
		inc_nlink(inode);
	}

	if (tmpfile)
		d_mark_tmpfile(tmpfile, inode);

	d_instantiate(dentry, inode);
	if (inode != oip.newinode) {
		pr_warn_ratelimited("newly created inode found in cache (%pd2)\n",
				    dentry);
	}

	/* Force lookup of new upper hardlink to find its lower */
	if (hardlink)
		d_drop(dentry);

	return 0;
}

static bool ovl_type_merge(struct dentry *dentry)
{
	return OVL_TYPE_MERGE(ovl_path_type(dentry));
}

static bool ovl_type_origin(struct dentry *dentry)
{
	return OVL_TYPE_ORIGIN(ovl_path_type(dentry));
}

static int ovl_create_upper(struct dentry *dentry, struct inode *inode,
			    struct ovl_cattr *attr)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *upperdir = ovl_dentry_upper(dentry->d_parent);
	struct inode *udir = upperdir->d_inode;
	struct dentry *newdentry;
	int err;

	inode_lock_nested(udir, I_MUTEX_PARENT);
	newdentry = ovl_create_real(ofs, upperdir,
				    ovl_lookup_upper(ofs, dentry->d_name.name,
						     upperdir, dentry->d_name.len),
				    attr);
	inode_unlock(udir);
	if (IS_ERR(newdentry))
		return PTR_ERR(newdentry);

	if (ovl_type_merge(dentry->d_parent) && d_is_dir(newdentry) &&
	    !ovl_allow_offline_changes(ofs)) {
		/* Setting opaque here is just an optimization, allow to fail */
		ovl_set_opaque(dentry, newdentry);
	}

	ovl_dir_modified(dentry->d_parent, false);
	err = ovl_instantiate(dentry, inode, newdentry, !!attr->hardlink, NULL);
	if (err)
		goto out_cleanup;
	return 0;

out_cleanup:
	ovl_cleanup(ofs, upperdir, newdentry);
	dput(newdentry);
	return err;
}

static struct dentry *ovl_clear_empty(struct dentry *dentry,
				      struct list_head *list)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *workdir = ovl_workdir(dentry);
	struct dentry *upperdir = ovl_dentry_upper(dentry->d_parent);
	struct path upperpath;
	struct dentry *upper;
	struct dentry *opaquedir;
	struct kstat stat;
	int err;

	if (WARN_ON(!workdir))
		return ERR_PTR(-EROFS);

	ovl_path_upper(dentry, &upperpath);
	err = vfs_getattr(&upperpath, &stat,
			  STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (err)
		goto out;

	err = -ESTALE;
	if (!S_ISDIR(stat.mode))
		goto out;
	upper = upperpath.dentry;

	opaquedir = ovl_create_temp(ofs, workdir, OVL_CATTR(stat.mode));
	err = PTR_ERR(opaquedir);
	if (IS_ERR(opaquedir))
		goto out;

	err = ovl_lock_rename_workdir(workdir, opaquedir, upperdir, upper);
	if (err)
		goto out_cleanup_unlocked;

	err = ovl_copy_xattr(dentry->d_sb, &upperpath, opaquedir);
	if (err)
		goto out_cleanup;

	err = ovl_set_opaque(dentry, opaquedir);
	if (err)
		goto out_cleanup;

	inode_lock(opaquedir->d_inode);
	err = ovl_set_attr(ofs, opaquedir, &stat);
	inode_unlock(opaquedir->d_inode);
	if (err)
		goto out_cleanup;

	err = ovl_do_rename(ofs, workdir, opaquedir, upperdir, upper, RENAME_EXCHANGE);
	unlock_rename(workdir, upperdir);
	if (err)
		goto out_cleanup_unlocked;

	ovl_cleanup_whiteouts(ofs, upper, list);
	ovl_cleanup(ofs, workdir, upper);

	/* dentry's upper doesn't match now, get rid of it */
	d_drop(dentry);

	return opaquedir;

out_cleanup:
	unlock_rename(workdir, upperdir);
out_cleanup_unlocked:
	ovl_cleanup(ofs, workdir, opaquedir);
	dput(opaquedir);
out:
	return ERR_PTR(err);
}

static int ovl_set_upper_acl(struct ovl_fs *ofs, struct dentry *upperdentry,
			     const char *acl_name, struct posix_acl *acl)
{
	if (!IS_ENABLED(CONFIG_FS_POSIX_ACL) || !acl)
		return 0;

	return ovl_do_set_acl(ofs, upperdentry, acl_name, acl);
}

static int ovl_create_over_whiteout(struct dentry *dentry, struct inode *inode,
				    struct ovl_cattr *cattr)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *workdir = ovl_workdir(dentry);
	struct dentry *upperdir = ovl_dentry_upper(dentry->d_parent);
	struct dentry *upper;
	struct dentry *newdentry;
	int err;
	struct posix_acl *acl, *default_acl;
	bool hardlink = !!cattr->hardlink;

	if (WARN_ON(!workdir))
		return -EROFS;

	if (!hardlink) {
		err = posix_acl_create(dentry->d_parent->d_inode,
				       &cattr->mode, &default_acl, &acl);
		if (err)
			return err;
	}

	upper = ovl_lookup_upper_unlocked(ofs, dentry->d_name.name, upperdir,
					  dentry->d_name.len);
	err = PTR_ERR(upper);
	if (IS_ERR(upper))
		goto out;

	err = -ESTALE;
	if (d_is_negative(upper) || !ovl_upper_is_whiteout(ofs, upper))
		goto out_dput;

	newdentry = ovl_create_temp(ofs, workdir, cattr);
	err = PTR_ERR(newdentry);
	if (IS_ERR(newdentry))
		goto out_dput;

	err = ovl_lock_rename_workdir(workdir, newdentry, upperdir, upper);
	if (err)
		goto out_cleanup_unlocked;

	/*
	 * mode could have been mutilated due to umask (e.g. sgid directory)
	 */
	if (!hardlink &&
	    !S_ISLNK(cattr->mode) &&
	    newdentry->d_inode->i_mode != cattr->mode) {
		struct iattr attr = {
			.ia_valid = ATTR_MODE,
			.ia_mode = cattr->mode,
		};
		inode_lock(newdentry->d_inode);
		err = ovl_do_notify_change(ofs, newdentry, &attr);
		inode_unlock(newdentry->d_inode);
		if (err)
			goto out_cleanup;
	}
	if (!hardlink) {
		err = ovl_set_upper_acl(ofs, newdentry,
					XATTR_NAME_POSIX_ACL_ACCESS, acl);
		if (err)
			goto out_cleanup;

		err = ovl_set_upper_acl(ofs, newdentry,
					XATTR_NAME_POSIX_ACL_DEFAULT, default_acl);
		if (err)
			goto out_cleanup;
	}

	if (!hardlink && S_ISDIR(cattr->mode)) {
		err = ovl_set_opaque(dentry, newdentry);
		if (err)
			goto out_cleanup;

		err = ovl_do_rename(ofs, workdir, newdentry, upperdir, upper,
				    RENAME_EXCHANGE);
		unlock_rename(workdir, upperdir);
		if (err)
			goto out_cleanup_unlocked;

		ovl_cleanup(ofs, workdir, upper);
	} else {
		err = ovl_do_rename(ofs, workdir, newdentry, upperdir, upper, 0);
		unlock_rename(workdir, upperdir);
		if (err)
			goto out_cleanup_unlocked;
	}
	ovl_dir_modified(dentry->d_parent, false);
	err = ovl_instantiate(dentry, inode, newdentry, hardlink, NULL);
	if (err) {
		ovl_cleanup(ofs, upperdir, newdentry);
		dput(newdentry);
	}
out_dput:
	dput(upper);
out:
	if (!hardlink) {
		posix_acl_release(acl);
		posix_acl_release(default_acl);
	}
	return err;

out_cleanup:
	unlock_rename(workdir, upperdir);
out_cleanup_unlocked:
	ovl_cleanup(ofs, workdir, newdentry);
	dput(newdentry);
	goto out_dput;
}

static const struct cred *ovl_setup_cred_for_create(struct dentry *dentry,
						    struct inode *inode,
						    umode_t mode,
						    const struct cred *old_cred)
{
	int err;
	struct cred *override_cred;

	override_cred = prepare_creds();
	if (!override_cred)
		return ERR_PTR(-ENOMEM);

	override_cred->fsuid = inode->i_uid;
	override_cred->fsgid = inode->i_gid;
	err = security_dentry_create_files_as(dentry, mode, &dentry->d_name,
					      old_cred, override_cred);
	if (err) {
		put_cred(override_cred);
		return ERR_PTR(err);
	}

	/*
	 * Caller is going to match this with revert_creds() and drop
	 * referenec on the returned creds.
	 * We must be called with creator creds already, otherwise we risk
	 * leaking creds.
	 */
	old_cred = override_creds(override_cred);
	WARN_ON_ONCE(old_cred != ovl_creds(dentry->d_sb));

	return override_cred;
}

static int ovl_create_or_link(struct dentry *dentry, struct inode *inode,
			      struct ovl_cattr *attr, bool origin)
{
	int err;
	const struct cred *old_cred, *new_cred = NULL;
	struct dentry *parent = dentry->d_parent;

	old_cred = ovl_override_creds(dentry->d_sb);

	/*
	 * When linking a file with copy up origin into a new parent, mark the
	 * new parent dir "impure".
	 */
	if (origin) {
		err = ovl_set_impure(parent, ovl_dentry_upper(parent));
		if (err)
			goto out_revert_creds;
	}

	if (!attr->hardlink) {
		/*
		 * In the creation cases(create, mkdir, mknod, symlink),
		 * ovl should transfer current's fs{u,g}id to underlying
		 * fs. Because underlying fs want to initialize its new
		 * inode owner using current's fs{u,g}id. And in this
		 * case, the @inode is a new inode that is initialized
		 * in inode_init_owner() to current's fs{u,g}id. So use
		 * the inode's i_{u,g}id to override the cred's fs{u,g}id.
		 *
		 * But in the other hardlink case, ovl_link() does not
		 * create a new inode, so just use the ovl mounter's
		 * fs{u,g}id.
		 */
		new_cred = ovl_setup_cred_for_create(dentry, inode, attr->mode,
						     old_cred);
		err = PTR_ERR(new_cred);
		if (IS_ERR(new_cred)) {
			new_cred = NULL;
			goto out_revert_creds;
		}
	}

	if (!ovl_dentry_is_whiteout(dentry))
		err = ovl_create_upper(dentry, inode, attr);
	else
		err = ovl_create_over_whiteout(dentry, inode, attr);

out_revert_creds:
	ovl_revert_creds(old_cred);
	put_cred(new_cred);
	return err;
}

static int ovl_create_object(struct dentry *dentry, int mode, dev_t rdev,
			     const char *link)
{
	int err;
	struct inode *inode;
	struct ovl_cattr attr = {
		.rdev = rdev,
		.link = link,
	};

	err = ovl_copy_up(dentry->d_parent);
	if (err)
		return err;

	err = ovl_want_write(dentry);
	if (err)
		goto out;

	/* Preallocate inode to be used by ovl_get_inode() */
	err = -ENOMEM;
	inode = ovl_new_inode(dentry->d_sb, mode, rdev);
	if (!inode)
		goto out_drop_write;

	spin_lock(&inode->i_lock);
	inode->i_state |= I_CREATING;
	spin_unlock(&inode->i_lock);

	inode_init_owner(&nop_mnt_idmap, inode, dentry->d_parent->d_inode, mode);
	attr.mode = inode->i_mode;

	err = ovl_create_or_link(dentry, inode, &attr, false);
	/* Did we end up using the preallocated inode? */
	if (inode != d_inode(dentry))
		iput(inode);

out_drop_write:
	ovl_drop_write(dentry);
out:
	return err;
}

static int ovl_create(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, bool excl)
{
	return ovl_create_object(dentry, (mode & 07777) | S_IFREG, 0, NULL);
}

static struct dentry *ovl_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				struct dentry *dentry, umode_t mode)
{
	return ERR_PTR(ovl_create_object(dentry, (mode & 07777) | S_IFDIR, 0, NULL));
}

static int ovl_mknod(struct mnt_idmap *idmap, struct inode *dir,
		     struct dentry *dentry, umode_t mode, dev_t rdev)
{
	/* Don't allow creation of "whiteout" on overlay */
	if (S_ISCHR(mode) && rdev == WHITEOUT_DEV)
		return -EPERM;

	return ovl_create_object(dentry, mode, rdev, NULL);
}

static int ovl_symlink(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, const char *link)
{
	return ovl_create_object(dentry, S_IFLNK, 0, link);
}

static int ovl_set_link_redirect(struct dentry *dentry)
{
	const struct cred *old_cred;
	int err;

	old_cred = ovl_override_creds(dentry->d_sb);
	err = ovl_set_redirect(dentry, false);
	ovl_revert_creds(old_cred);

	return err;
}

static int ovl_link(struct dentry *old, struct inode *newdir,
		    struct dentry *new)
{
	int err;
	struct inode *inode;

	err = ovl_copy_up(old);
	if (err)
		goto out;

	err = ovl_copy_up(new->d_parent);
	if (err)
		goto out;

	err = ovl_nlink_start(old);
	if (err)
		goto out;

	if (ovl_is_metacopy_dentry(old)) {
		err = ovl_set_link_redirect(old);
		if (err)
			goto out_nlink_end;
	}

	inode = d_inode(old);
	ihold(inode);

	err = ovl_create_or_link(new, inode,
			&(struct ovl_cattr) {.hardlink = ovl_dentry_upper(old)},
			ovl_type_origin(old));
	if (err)
		iput(inode);

out_nlink_end:
	ovl_nlink_end(old);
out:
	return err;
}

static bool ovl_matches_upper(struct dentry *dentry, struct dentry *upper)
{
	return d_inode(ovl_dentry_upper(dentry)) == d_inode(upper);
}

static int ovl_remove_and_whiteout(struct dentry *dentry,
				   struct list_head *list)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *workdir = ovl_workdir(dentry);
	struct dentry *upperdir = ovl_dentry_upper(dentry->d_parent);
	struct dentry *upper;
	struct dentry *opaquedir = NULL;
	int err;

	if (WARN_ON(!workdir))
		return -EROFS;

	if (!list_empty(list)) {
		opaquedir = ovl_clear_empty(dentry, list);
		err = PTR_ERR(opaquedir);
		if (IS_ERR(opaquedir))
			goto out;
	}

	upper = ovl_lookup_upper_unlocked(ofs, dentry->d_name.name, upperdir,
					  dentry->d_name.len);
	err = PTR_ERR(upper);
	if (IS_ERR(upper))
		goto out_dput;

	err = -ESTALE;
	if ((opaquedir && upper != opaquedir) ||
	    (!opaquedir && ovl_dentry_upper(dentry) &&
	     !ovl_matches_upper(dentry, upper))) {
		goto out_dput_upper;
	}

	err = ovl_cleanup_and_whiteout(ofs, upperdir, upper);
	if (!err)
		ovl_dir_modified(dentry->d_parent, true);

	d_drop(dentry);
out_dput_upper:
	dput(upper);
out_dput:
	dput(opaquedir);
out:
	return err;
}

static int ovl_remove_upper(struct dentry *dentry, bool is_dir,
			    struct list_head *list)
{
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	struct dentry *upperdir = ovl_dentry_upper(dentry->d_parent);
	struct inode *dir = upperdir->d_inode;
	struct dentry *upper;
	struct dentry *opaquedir = NULL;
	int err;

	if (!list_empty(list)) {
		opaquedir = ovl_clear_empty(dentry, list);
		err = PTR_ERR(opaquedir);
		if (IS_ERR(opaquedir))
			goto out;
	}

	inode_lock_nested(dir, I_MUTEX_PARENT);
	upper = ovl_lookup_upper(ofs, dentry->d_name.name, upperdir,
				 dentry->d_name.len);
	err = PTR_ERR(upper);
	if (IS_ERR(upper))
		goto out_unlock;

	err = -ESTALE;
	if ((opaquedir && upper != opaquedir) ||
	    (!opaquedir && !ovl_matches_upper(dentry, upper)))
		goto out_dput_upper;

	if (is_dir)
		err = ovl_do_rmdir(ofs, dir, upper);
	else
		err = ovl_do_unlink(ofs, dir, upper);
	ovl_dir_modified(dentry->d_parent, ovl_type_origin(dentry));

	/*
	 * Keeping this dentry hashed would mean having to release
	 * upperpath/lowerpath, which could only be done if we are the
	 * sole user of this dentry.  Too tricky...  Just unhash for
	 * now.
	 */
	if (!err)
		d_drop(dentry);
out_dput_upper:
	dput(upper);
out_unlock:
	inode_unlock(dir);
	dput(opaquedir);
out:
	return err;
}

static bool ovl_pure_upper(struct dentry *dentry)
{
	return !ovl_dentry_lower(dentry) &&
	       !ovl_test_flag(OVL_WHITEOUTS, d_inode(dentry));
}

static void ovl_drop_nlink(struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct dentry *alias;

	/* Try to find another, hashed alias */
	spin_lock(&inode->i_lock);
	hlist_for_each_entry(alias, &inode->i_dentry, d_u.d_alias) {
		if (alias != dentry && !d_unhashed(alias))
			break;
	}
	spin_unlock(&inode->i_lock);

	/*
	 * Changes to underlying layers may cause i_nlink to lose sync with
	 * reality.  In this case prevent the link count from going to zero
	 * prematurely.
	 */
	if (inode->i_nlink > !!alias)
		drop_nlink(inode);
}

static int ovl_do_remove(struct dentry *dentry, bool is_dir)
{
	int err;
	const struct cred *old_cred;
	bool lower_positive = ovl_lower_positive(dentry);
	LIST_HEAD(list);

	/* No need to clean pure upper removed by vfs_rmdir() */
	if (is_dir && (lower_positive || !ovl_pure_upper(dentry))) {
		err = ovl_check_empty_dir(dentry, &list);
		if (err)
			goto out;
	}

	err = ovl_copy_up(dentry->d_parent);
	if (err)
		goto out;

	err = ovl_nlink_start(dentry);
	if (err)
		goto out;

	old_cred = ovl_override_creds(dentry->d_sb);
	if (!lower_positive)
		err = ovl_remove_upper(dentry, is_dir, &list);
	else
		err = ovl_remove_and_whiteout(dentry, &list);
	ovl_revert_creds(old_cred);
	if (!err) {
		if (is_dir)
			clear_nlink(dentry->d_inode);
		else
			ovl_drop_nlink(dentry);
	}
	ovl_nlink_end(dentry);

	/*
	 * Copy ctime
	 *
	 * Note: we fail to update ctime if there was no copy-up, only a
	 * whiteout
	 */
	if (ovl_dentry_upper(dentry))
		ovl_copyattr(d_inode(dentry));

out:
	ovl_cache_free(&list);
	return err;
}

static int ovl_unlink(struct inode *dir, struct dentry *dentry)
{
	return ovl_do_remove(dentry, false);
}

static int ovl_rmdir(struct inode *dir, struct dentry *dentry)
{
	return ovl_do_remove(dentry, true);
}

static bool ovl_type_merge_or_lower(struct dentry *dentry)
{
	enum ovl_path_type type = ovl_path_type(dentry);

	return OVL_TYPE_MERGE(type) || !OVL_TYPE_UPPER(type);
}

static bool ovl_can_move(struct dentry *dentry)
{
	return ovl_redirect_dir(OVL_FS(dentry->d_sb)) ||
		!d_is_dir(dentry) || !ovl_type_merge_or_lower(dentry);
}

static char *ovl_get_redirect(struct dentry *dentry, bool abs_redirect)
{
	char *buf, *ret;
	struct dentry *d, *tmp;
	int buflen = ovl_redirect_max + 1;

	if (!abs_redirect) {
		ret = kstrndup(dentry->d_name.name, dentry->d_name.len,
			       GFP_KERNEL);
		goto out;
	}

	buf = ret = kmalloc(buflen, GFP_KERNEL);
	if (!buf)
		goto out;

	buflen--;
	buf[buflen] = '\0';
	for (d = dget(dentry); !IS_ROOT(d);) {
		const char *name;
		int thislen;

		spin_lock(&d->d_lock);
		name = ovl_dentry_get_redirect(d);
		if (name) {
			thislen = strlen(name);
		} else {
			name = d->d_name.name;
			thislen = d->d_name.len;
		}

		/* If path is too long, fall back to userspace move */
		if (thislen + (name[0] != '/') > buflen) {
			ret = ERR_PTR(-EXDEV);
			spin_unlock(&d->d_lock);
			goto out_put;
		}

		buflen -= thislen;
		memcpy(&buf[buflen], name, thislen);
		spin_unlock(&d->d_lock);
		tmp = dget_parent(d);

		dput(d);
		d = tmp;

		/* Absolute redirect: finished */
		if (buf[buflen] == '/')
			break;
		buflen--;
		buf[buflen] = '/';
	}
	ret = kstrdup(&buf[buflen], GFP_KERNEL);
out_put:
	dput(d);
	kfree(buf);
out:
	return ret ? ret : ERR_PTR(-ENOMEM);
}

static bool ovl_need_absolute_redirect(struct dentry *dentry, bool samedir)
{
	struct dentry *lowerdentry;

	if (!samedir)
		return true;

	if (d_is_dir(dentry))
		return false;

	/*
	 * For non-dir hardlinked files, we need absolute redirects
	 * in general as two upper hardlinks could be in different
	 * dirs. We could put a relative redirect now and convert
	 * it to absolute redirect later. But when nlink > 1 and
	 * indexing is on, that means relative redirect needs to be
	 * converted to absolute during copy up of another lower
	 * hardllink as well.
	 *
	 * So without optimizing too much, just check if lower is
	 * a hard link or not. If lower is hard link, put absolute
	 * redirect.
	 */
	lowerdentry = ovl_dentry_lower(dentry);
	return (d_inode(lowerdentry)->i_nlink > 1);
}

static int ovl_set_redirect(struct dentry *dentry, bool samedir)
{
	int err;
	struct ovl_fs *ofs = OVL_FS(dentry->d_sb);
	const char *redirect = ovl_dentry_get_redirect(dentry);
	bool absolute_redirect = ovl_need_absolute_redirect(dentry, samedir);

	if (redirect && (!absolute_redirect || redirect[0] == '/'))
		return 0;

	redirect = ovl_get_redirect(dentry, absolute_redirect);
	if (IS_ERR(redirect))
		return PTR_ERR(redirect);

	err = ovl_check_setxattr(ofs, ovl_dentry_upper(dentry),
				 OVL_XATTR_REDIRECT,
				 redirect, strlen(redirect), -EXDEV);
	if (!err) {
		spin_lock(&dentry->d_lock);
		ovl_dentry_set_redirect(dentry, redirect);
		spin_unlock(&dentry->d_lock);
	} else {
		kfree(redirect);
		pr_warn_ratelimited("failed to set redirect (%i)\n",
				    err);
		/* Fall back to userspace copy-up */
		err = -EXDEV;
	}
	return err;
}

static int ovl_rename(struct mnt_idmap *idmap, struct inode *olddir,
		      struct dentry *old, struct inode *newdir,
		      struct dentry *new, unsigned int flags)
{
	int err;
	struct dentry *old_upperdir;
	struct dentry *new_upperdir;
	struct dentry *olddentry = NULL;
	struct dentry *newdentry = NULL;
	struct dentry *trap, *de;
	bool old_opaque;
	bool new_opaque;
	bool cleanup_whiteout = false;
	bool update_nlink = false;
	bool overwrite = !(flags & RENAME_EXCHANGE);
	bool is_dir = d_is_dir(old);
	bool new_is_dir = d_is_dir(new);
	bool samedir = olddir == newdir;
	struct dentry *opaquedir = NULL;
	const struct cred *old_cred = NULL;
	struct ovl_fs *ofs = OVL_FS(old->d_sb);
	LIST_HEAD(list);

	err = -EINVAL;
	if (flags & ~(RENAME_EXCHANGE | RENAME_NOREPLACE))
		goto out;

	flags &= ~RENAME_NOREPLACE;

	/* Don't copy up directory trees */
	err = -EXDEV;
	if (!ovl_can_move(old))
		goto out;
	if (!overwrite && !ovl_can_move(new))
		goto out;

	if (overwrite && new_is_dir && !ovl_pure_upper(new)) {
		err = ovl_check_empty_dir(new, &list);
		if (err)
			goto out;
	}

	if (overwrite) {
		if (ovl_lower_positive(old)) {
			if (!ovl_dentry_is_whiteout(new)) {
				/* Whiteout source */
				flags |= RENAME_WHITEOUT;
			} else {
				/* Switch whiteouts */
				flags |= RENAME_EXCHANGE;
			}
		} else if (is_dir && ovl_dentry_is_whiteout(new)) {
			flags |= RENAME_EXCHANGE;
			cleanup_whiteout = true;
		}
	}

	err = ovl_copy_up(old);
	if (err)
		goto out;

	err = ovl_copy_up(new->d_parent);
	if (err)
		goto out;
	if (!overwrite) {
		err = ovl_copy_up(new);
		if (err)
			goto out;
	} else if (d_inode(new)) {
		err = ovl_nlink_start(new);
		if (err)
			goto out;

		update_nlink = true;
	}

	if (!update_nlink) {
		/* ovl_nlink_start() took ovl_want_write() */
		err = ovl_want_write(old);
		if (err)
			goto out;
	}

	old_cred = ovl_override_creds(old->d_sb);

	if (!list_empty(&list)) {
		opaquedir = ovl_clear_empty(new, &list);
		err = PTR_ERR(opaquedir);
		if (IS_ERR(opaquedir)) {
			opaquedir = NULL;
			goto out_revert_creds;
		}
	}

	old_upperdir = ovl_dentry_upper(old->d_parent);
	new_upperdir = ovl_dentry_upper(new->d_parent);

	if (!samedir) {
		/*
		 * When moving a merge dir or non-dir with copy up origin into
		 * a new parent, we are marking the new parent dir "impure".
		 * When ovl_iterate() iterates an "impure" upper dir, it will
		 * lookup the origin inodes of the entries to fill d_ino.
		 */
		if (ovl_type_origin(old)) {
			err = ovl_set_impure(new->d_parent, new_upperdir);
			if (err)
				goto out_revert_creds;
		}
		if (!overwrite && ovl_type_origin(new)) {
			err = ovl_set_impure(old->d_parent, old_upperdir);
			if (err)
				goto out_revert_creds;
		}
	}

	trap = lock_rename(new_upperdir, old_upperdir);
	if (IS_ERR(trap)) {
		err = PTR_ERR(trap);
		goto out_revert_creds;
	}

	de = ovl_lookup_upper(ofs, old->d_name.name, old_upperdir,
			      old->d_name.len);
	err = PTR_ERR(de);
	if (IS_ERR(de))
		goto out_unlock;
	olddentry = de;

	err = -ESTALE;
	if (!ovl_matches_upper(old, olddentry))
		goto out_unlock;

	de = ovl_lookup_upper(ofs, new->d_name.name, new_upperdir,
			      new->d_name.len);
	err = PTR_ERR(de);
	if (IS_ERR(de))
		goto out_unlock;
	newdentry = de;

	old_opaque = ovl_dentry_is_opaque(old);
	new_opaque = ovl_dentry_is_opaque(new);

	err = -ESTALE;
	if (d_inode(new) && ovl_dentry_upper(new)) {
		if (opaquedir) {
			if (newdentry != opaquedir)
				goto out_unlock;
		} else {
			if (!ovl_matches_upper(new, newdentry))
				goto out_unlock;
		}
	} else {
		if (!d_is_negative(newdentry)) {
			if (!new_opaque || !ovl_upper_is_whiteout(ofs, newdentry))
				goto out_unlock;
		} else {
			if (flags & RENAME_EXCHANGE)
				goto out_unlock;
		}
	}

	if (olddentry == trap)
		goto out_unlock;
	if (newdentry == trap)
		goto out_unlock;

	if (olddentry->d_inode == newdentry->d_inode)
		goto out_unlock;

	err = 0;
	if (ovl_type_merge_or_lower(old))
		err = ovl_set_redirect(old, samedir);
	else if (is_dir && !old_opaque && ovl_type_merge(new->d_parent))
		err = ovl_set_opaque_xerr(old, olddentry, -EXDEV);
	if (err)
		goto out_unlock;

	if (!overwrite && ovl_type_merge_or_lower(new))
		err = ovl_set_redirect(new, samedir);
	else if (!overwrite && new_is_dir && !new_opaque &&
		 ovl_type_merge(old->d_parent))
		err = ovl_set_opaque_xerr(new, newdentry, -EXDEV);
	if (err)
		goto out_unlock;

	err = ovl_do_rename(ofs, old_upperdir, olddentry,
			    new_upperdir, newdentry, flags);
	unlock_rename(new_upperdir, old_upperdir);
	if (err)
		goto out_revert_creds;

	if (cleanup_whiteout)
		ovl_cleanup(ofs, old_upperdir, newdentry);

	if (overwrite && d_inode(new)) {
		if (new_is_dir)
			clear_nlink(d_inode(new));
		else
			ovl_drop_nlink(new);
	}

	ovl_dir_modified(old->d_parent, ovl_type_origin(old) ||
			 (!overwrite && ovl_type_origin(new)));
	ovl_dir_modified(new->d_parent, ovl_type_origin(old) ||
			 (d_inode(new) && ovl_type_origin(new)));

	/* copy ctime: */
	ovl_copyattr(d_inode(old));
	if (d_inode(new) && ovl_dentry_upper(new))
		ovl_copyattr(d_inode(new));

out_revert_creds:
	ovl_revert_creds(old_cred);
	if (update_nlink)
		ovl_nlink_end(new);
	else
		ovl_drop_write(old);
out:
	dput(newdentry);
	dput(olddentry);
	dput(opaquedir);
	ovl_cache_free(&list);
	return err;

out_unlock:
	unlock_rename(new_upperdir, old_upperdir);
	goto out_revert_creds;
}

static int ovl_create_tmpfile(struct file *file, struct dentry *dentry,
			      struct inode *inode, umode_t mode)
{
	const struct cred *old_cred, *new_cred = NULL;
	struct path realparentpath;
	struct file *realfile;
	struct ovl_file *of;
	struct dentry *newdentry;
	/* It's okay to set O_NOATIME, since the owner will be current fsuid */
	int flags = file->f_flags | OVL_OPEN_FLAGS;
	int err;

	old_cred = ovl_override_creds(dentry->d_sb);
	new_cred = ovl_setup_cred_for_create(dentry, inode, mode, old_cred);
	err = PTR_ERR(new_cred);
	if (IS_ERR(new_cred)) {
		new_cred = NULL;
		goto out_revert_creds;
	}

	ovl_path_upper(dentry->d_parent, &realparentpath);
	realfile = backing_tmpfile_open(&file->f_path, flags, &realparentpath,
					mode, current_cred());
	err = PTR_ERR_OR_ZERO(realfile);
	pr_debug("tmpfile/open(%pd2, 0%o) = %i\n", realparentpath.dentry, mode, err);
	if (err)
		goto out_revert_creds;

	of = ovl_file_alloc(realfile);
	if (!of) {
		fput(realfile);
		err = -ENOMEM;
		goto out_revert_creds;
	}

	/* ovl_instantiate() consumes the newdentry reference on success */
	newdentry = dget(realfile->f_path.dentry);
	err = ovl_instantiate(dentry, inode, newdentry, false, file);
	if (!err) {
		file->private_data = of;
	} else {
		dput(newdentry);
		ovl_file_free(of);
	}
out_revert_creds:
	ovl_revert_creds(old_cred);
	put_cred(new_cred);
	return err;
}

static int ovl_dummy_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int ovl_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
		       struct file *file, umode_t mode)
{
	int err;
	struct dentry *dentry = file->f_path.dentry;
	struct inode *inode;

	if (!OVL_FS(dentry->d_sb)->tmpfile)
		return -EOPNOTSUPP;

	err = ovl_copy_up(dentry->d_parent);
	if (err)
		return err;

	err = ovl_want_write(dentry);
	if (err)
		return err;

	err = -ENOMEM;
	inode = ovl_new_inode(dentry->d_sb, mode, 0);
	if (!inode)
		goto drop_write;

	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	err = ovl_create_tmpfile(file, dentry, inode, inode->i_mode);
	if (err)
		goto put_inode;

	/*
	 * Check if the preallocated inode was actually used.  Having something
	 * else assigned to the dentry shouldn't happen as that would indicate
	 * that the backing tmpfile "leaked" out of overlayfs.
	 */
	err = -EIO;
	if (WARN_ON(inode != d_inode(dentry)))
		goto put_realfile;

	/* inode reference was transferred to dentry */
	inode = NULL;
	err = finish_open(file, dentry, ovl_dummy_open);
put_realfile:
	/* Without FMODE_OPENED ->release() won't be called on @file */
	if (!(file->f_mode & FMODE_OPENED))
		ovl_file_free(file->private_data);
put_inode:
	iput(inode);
drop_write:
	ovl_drop_write(dentry);
	return err;
}

const struct inode_operations ovl_dir_inode_operations = {
	.lookup		= ovl_lookup,
	.mkdir		= ovl_mkdir,
	.symlink	= ovl_symlink,
	.unlink		= ovl_unlink,
	.rmdir		= ovl_rmdir,
	.rename		= ovl_rename,
	.link		= ovl_link,
	.setattr	= ovl_setattr,
	.create		= ovl_create,
	.mknod		= ovl_mknod,
	.permission	= ovl_permission,
	.getattr	= ovl_getattr,
	.listxattr	= ovl_listxattr,
	.get_inode_acl	= ovl_get_inode_acl,
	.get_acl	= ovl_get_acl,
	.set_acl	= ovl_set_acl,
	.update_time	= ovl_update_time,
	.fileattr_get	= ovl_fileattr_get,
	.fileattr_set	= ovl_fileattr_set,
	.tmpfile	= ovl_tmpfile,
};
