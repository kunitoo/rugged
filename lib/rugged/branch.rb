module Rugged
  class Branch < Rugged::Reference
    def self.each(repository, filter = nil, &block)
      warn "DEPRECATION WARNING: Rugged::Branch.each is deprecated and will be removed."
      repository.branches.each(filter, &block)
    end

    def self.each_name(repository, filter = nil, &block)
      warn "DEPRECATION WARNING: Rugged::Branch.each_name is deprecated and will be removed."
      repository.branches.each_name(filter, &block)
    end

    def self.lookup(repository, name, branch_type = :local)
      warn "DEPRECATION WARNING: Rugged::Branch.lookup is deprecated and will be removed."
      repository.branches[name, branch_type]
    end

    def self.create(repository, name, target, force = false)
      warn "DEPRECATION WARNING: Rugged::Branch.create is deprecated and will be removed."
      repository.branches.create(name, target, force)
    end

    # The object pointed at by the tip of this branch
    def tip
      @owner.lookup(self.resolve.target)
    end

    def ==(other)
      other.instance_of?(Rugged::Branch) &&
        other.canonical_name == self.canonical_name
    end
    
    # Get the remote the branch belongs to.
    #
    # If the branch is remote returns the remote it belongs to.
    # In case of local branch, it returns the remote of the branch
    # it tracks or nil if there is no tracking branch.
    #
    def remote
      remote_name = self.remote_name
      Rugged::Remote.lookup(@owner, remote_name) if remote_name
    end
  end
end
