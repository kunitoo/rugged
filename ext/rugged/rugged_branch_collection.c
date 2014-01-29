/*
 * The MIT License
 *
 * Copyright (c) 2014 GitHub, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "rugged.h"

extern VALUE rb_mRugged;
extern VALUE rb_cRuggedRepo;
extern VALUE rb_cRuggedBranch;

VALUE rb_cRuggedBranchCollection;

static inline VALUE rugged_branch_new(VALUE owner, git_reference *ref)
{
	return rugged_ref_new(rb_cRuggedBranch, owner, ref);
}

static inline int rugged_branch_lookup(git_reference **branch, git_repository *repo, VALUE rb_name_or_branch)
{
	if (rb_obj_is_kind_of(rb_name_or_branch, rb_cRuggedBranch)) {
		rb_name_or_branch = rb_funcall(rb_name_or_branch, rb_intern("canonical_name"), 0);

		if (TYPE(rb_name_or_branch) != T_STRING)
			rb_raise(rb_eTypeError, "Expected #canonical_name to return a String");

		return git_reference_lookup(branch, repo, StringValueCStr(rb_name_or_branch));
	} else if (TYPE(rb_name_or_branch) == T_STRING) {
		char *branch_name = StringValueCStr(rb_name_or_branch), *ref_name;
		int error;

		if (strncmp(branch_name, "refs/heads/", strlen("refs/heads/")) == 0 ||
		    strncmp(branch_name, "refs/remotes/", strlen("refs/remotes/")) == 0 ||
		    strcmp(branch_name, "HEAD") == 0)
			return git_reference_lookup(branch, repo, branch_name);			

		if ((error = git_branch_lookup(branch, repo, branch_name, GIT_BRANCH_LOCAL)) == GIT_OK ||
		    error != GIT_ENOTFOUND)
			return error;

		if ((error = git_branch_lookup(branch, repo, branch_name, GIT_BRANCH_REMOTE)) == GIT_OK ||
		    error != GIT_ENOTFOUND)
			return error;

		ref_name = xmalloc((strlen(branch_name) + strlen("refs/") + 1)  * sizeof(char));
		strcpy(ref_name, "refs/");
		strcat(ref_name, branch_name);

		error = git_reference_lookup(branch, repo, ref_name);
		xfree(ref_name);

		return error;
	} else {
		rb_raise(rb_eTypeError, "Expecting a String or Rugged::Branch instance");		
	}
}

/*
 *  call-seq:
 *    BranchCollection.new(repo) -> refs
 */
static VALUE rb_git_branch_collection_initialize(VALUE self, VALUE repo)
{
	rugged_set_owner(self, repo);
	return self;
}

static git_branch_t parse_branch_type(VALUE rb_filter)
{
	ID id_filter;

	Check_Type(rb_filter, T_SYMBOL);
	id_filter = SYM2ID(rb_filter);

	if (id_filter == rb_intern("local")) {
		return GIT_BRANCH_LOCAL;
	} else if (id_filter == rb_intern("remote")) {
		return GIT_BRANCH_REMOTE;
	} else {
		rb_raise(rb_eTypeError,
			"Invalid branch filter. Expected `:remote`, `:local` or `nil`");
	}
}

/*
 *  call-seq:
 *    Branch.create(repository, name, target, force = false) -> branch
 *
 *  Create a new branch in +repository+, with the given +name+, and pointing
 *  to the +target+.
 *
 *  +name+ needs to be a branch name, not an absolute reference path
 *  (e.g. +development+ instead of +refs/heads/development+).
 *
 *  +target+ needs to be an existing commit in the given +repository+.
 *
 *  If +force+ is +true+, any existing branches will be overwritten.
 *
 *  Returns a Rugged::Branch object with the newly created branch.
 */
static VALUE rb_git_branch_collection_create(int argc, VALUE *argv, VALUE self)
{
	VALUE rb_repo = rugged_owner(self), rb_name, rb_target, rb_force;
	git_repository *repo;
	git_reference *branch;
	git_commit *target;
	int error, force = 0;

	rb_scan_args(argc, argv, "21", &rb_name, &rb_target, &rb_force);

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	Check_Type(rb_name, T_STRING);
	Check_Type(rb_target, T_STRING);

	if (!NIL_P(rb_force))
		force = rugged_parse_bool(rb_force);

	target = (git_commit *)rugged_object_get(repo, rb_target, GIT_OBJ_COMMIT);

	error = git_branch_create(&branch, repo, StringValueCStr(rb_name), target, force);
	git_commit_free(target);

	rugged_exception_check(error);

	return rugged_branch_new(rb_repo, branch);
}

/*
 *  call-seq:
 *    Branch.lookup(repository, name) -> branch
 *
 *  Lookup a branch in +repository+, with the given +name+.
 *
 *  +name+ needs to be a branch name, not an absolute reference path
 *  (e.g. +development+ instead of +/refs/heads/development+).
 *
 *  Returns the looked up branch, or +nil+ if the branch doesn't exist.
 */
static VALUE rb_git_branch_collection_aref(VALUE self, VALUE rb_name) {
	git_reference *branch;
	git_repository *repo;

	VALUE rb_repo = rugged_owner(self);
	int error;

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	Check_Type(rb_name, T_STRING);

	error = rugged_branch_lookup(&branch, repo, rb_name);
	if (error == GIT_ENOTFOUND)
		return Qnil;

	rugged_exception_check(error);
	return rugged_branch_new(rb_repo, branch);
}

static VALUE each_branch(int argc, VALUE *argv, VALUE self, int branch_names_only)
{
	VALUE rb_repo = rugged_owner(self), rb_filter;
	git_repository *repo;
	git_branch_iterator *iter;
	int error, exception = 0;
	git_branch_t filter = (GIT_BRANCH_LOCAL | GIT_BRANCH_REMOTE), branch_type;

	rb_scan_args(argc, argv, "01", &rb_filter);

	if (!rb_block_given_p()) {
		VALUE symbol = branch_names_only ? CSTR2SYM("each_name") : CSTR2SYM("each");
		return rb_funcall(self, rb_intern("to_enum"), 2, symbol, rb_filter);
	}

	rugged_check_repo(rb_repo);

	if (!NIL_P(rb_filter))
		filter = parse_branch_type(rb_filter);

	Data_Get_Struct(rb_repo, git_repository, repo);

	error = git_branch_iterator_new(&iter, repo, filter);

	if (branch_names_only) {
		git_reference *branch;
		while (!exception && (error = git_branch_next(&branch, &branch_type, iter)) == GIT_OK) {
			rb_protect(rb_yield, rb_str_new_utf8(git_reference_shorthand(branch)), &exception);
		}
	} else {
		git_reference *branch;
		while (!exception && (error = git_branch_next(&branch, &branch_type, iter)) == GIT_OK) {
			rb_protect(rb_yield, rugged_branch_new(rb_repo, branch), &exception);
		}
	}

	git_branch_iterator_free(iter);

	if (exception)
		rb_jump_tag(exception);

	if (error != GIT_ITEROVER)
		rugged_exception_check(error);

	return Qnil;
}

/*
 *  call-seq:
 *    branches.each(repository, filter = nil) { |branch| block }
 *    branches.each(repository, filter = nil) -> enumerator
 *
 *  Iterate through the branches in +repository+. Iteration can be
 *  optionally filtered to yield only +:local+ or +:remote+ branches.
 *
 *  The given block will be called once with a +Rugged::Branch+ object
 *  for each branch in the repository. If no block is given, an enumerator
 *  will be returned.
 */
static VALUE rb_git_branch_collection_each(int argc, VALUE *argv, VALUE self)
{
	return each_branch(argc, argv, self, 0);
}

/*
 *  call-seq:
 *    branches.each_name(repository, filter = nil) { |branch_name| block }
 *    branches.each_name(repository, filter = nil) -> enumerator
 *
 *  Iterate through the names of the branches in +repository+. Iteration can be
 *  optionally filtered to yield only +:local+ or +:remote+ branches.
 *
 *  The given block will be called once with the name of each branch as a +String+.
 *  If no block is given, an enumerator will be returned.
 */
static VALUE rb_git_branch_collection_each_name(int argc, VALUE *argv, VALUE self)
{
	return each_branch(argc, argv, self, 1);
}

/*
 *  call-seq:
 *    branches.delete(branch) -> nil
 *    branches.delete(name) -> nil
 *
 *  Remove a branch from the repository. The branch object will become invalidated
 *  and won't be able to be used for any other operations.
 */
static VALUE rb_git_branch_collection_delete(VALUE self, VALUE rb_name_or_branch)
{
	git_reference *branch;
	git_repository *repo;

	VALUE rb_repo = rugged_owner(self);
	int error;

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);

	error = rugged_branch_lookup(&branch, repo, rb_name_or_branch);
	rugged_exception_check(error);

	error = git_branch_delete(branch);
	git_reference_free(branch);
	rugged_exception_check(error);

	return Qnil;
}

/*
 *  call-seq:
 *    branch.move(old_name, new_name, force = false) -> new_branch
 *    branch.move(branch, new_name, force = false) -> new_branch
 *    branch.rename(old_name, new_name, force = false) -> new_branch
 *    branch.rename(branch, new_name, force = false) -> new_branch
 *
 *  Rename a branch to +new_name+.
 *
 *  +new_name+ needs to be a branch name, not an absolute reference path
 *  (e.g. +development+ instead of +refs/heads/development+).
 *
 *  If +force+ is +true+, the branch will be renamed even if a branch
 *  with +new_name+ already exists.
 *
 *  A new Rugged::Branch object for the renamed branch will be returned.
 */
static VALUE rb_git_branch_collection_move(int argc, VALUE *argv, VALUE self)
{
	VALUE rb_repo = rugged_owner(self), rb_name_or_branch, rb_new_branch_name, rb_force;
	git_reference *old_branch = NULL, *new_branch = NULL;
	git_repository *repo;
	int error, force = 0;

	rb_scan_args(argc, argv, "21", &rb_name_or_branch, &rb_new_branch_name, &rb_force);
	Check_Type(rb_new_branch_name, T_STRING);

	rugged_check_repo(rb_repo);
	Data_Get_Struct(rb_repo, git_repository, repo);
	
	error = rugged_branch_lookup(&old_branch, repo, rb_name_or_branch);
	rugged_exception_check(error);

	if (!NIL_P(rb_force))
		force = rugged_parse_bool(rb_force);

	error = git_branch_move(&new_branch, old_branch, StringValueCStr(rb_new_branch_name), force);
	git_reference_free(old_branch);
	rugged_exception_check(error);

	return rugged_branch_new(rugged_owner(self), new_branch);
}

/*
 *  call-seq:
 *    branches.exist?(name) -> true or false
 *    branches.exists?(name) -> true or false
 *
 *  Check if a given reference exists in the collection's +repository+.
 */
static VALUE rb_git_branch_collection_exist_p(VALUE self, VALUE rb_name)
{
	VALUE rb_repo = rugged_owner(self);
	git_repository *repo;
	git_reference *branch;
	int error;

	Check_Type(rb_name, T_STRING);
	Data_Get_Struct(rb_repo, git_repository, repo);

	error = rugged_branch_lookup(&branch, repo, rb_name);
	git_reference_free(branch);

	if (error == GIT_ENOTFOUND)
		return Qfalse;
	else
		rugged_exception_check(error);

	return Qtrue;
}

void Init_rugged_branch_collection(void)
{
	rb_cRuggedBranchCollection = rb_define_class_under(rb_mRugged, "BranchCollection", rb_cObject);
	rb_include_module(rb_cRuggedBranchCollection, rb_mEnumerable);

	rb_define_method(rb_cRuggedBranchCollection, "initialize", rb_git_branch_collection_initialize, 1);

	rb_define_method(rb_cRuggedBranchCollection, "[]",         rb_git_branch_collection_aref, 1);
	rb_define_method(rb_cRuggedBranchCollection, "create",     rb_git_branch_collection_create, -1);

	rb_define_method(rb_cRuggedBranchCollection, "each",       rb_git_branch_collection_each, -1);
	rb_define_method(rb_cRuggedBranchCollection, "each_name",  rb_git_branch_collection_each_name, -1);

	rb_define_method(rb_cRuggedBranchCollection, "exist?",     rb_git_branch_collection_exist_p, 1);
	rb_define_method(rb_cRuggedBranchCollection, "exists?",    rb_git_branch_collection_exist_p, 1);

	rb_define_method(rb_cRuggedBranchCollection, "move",       rb_git_branch_collection_move, -1);
	rb_define_method(rb_cRuggedBranchCollection, "rename",     rb_git_branch_collection_move, -1);
	// rb_define_method(rb_cRuggedBranchCollection, "update",     rb_git_branch_collection_update, -1);
	rb_define_method(rb_cRuggedBranchCollection, "delete",     rb_git_branch_collection_delete, 1);
}
