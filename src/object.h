/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_object_h__
#define INCLUDE_object_h__

/** Base git object for inheritance */
struct git_object {
	git_cached_obj cached;
	git_repository *repo;
	git_otype type;
};

/* fully free the object; internal method, DO NOT EXPORT */
void git_object__free(void *object);

GIT_INLINE(int) git_object__dup(git_object **dest, git_object *source)
{
	git_cached_obj_incref(source);
	*dest = source;
	return 0;
}

int git_object__from_odb_object(
	git_object **object_out,
	git_repository *repo,
	git_odb_object *odb_obj,
	git_otype type);

int git_object__resolve_to_type(git_object **obj, git_otype type);

int git_oid__parse(git_oid *oid, const char **buffer_out, const char *buffer_end, const char *header);

void git_oid__writebuf(git_buf *buf, const char *header, const git_oid *oid);

#endif
